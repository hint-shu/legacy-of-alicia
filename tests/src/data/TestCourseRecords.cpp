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

//! LOA (R75, #14): пер-курсовые рекорды персонажа — ХРАНЕНИЕ.
//!
//! Что здесь доказывается и почему именно это. Поля `courseRecords` /
//! `totalSpeedGames` / `totalMagicGames` — новые поля ЗАПИСИ ПЕРСОНАЖА, то есть
//! файла на диске, который переживает рестарт и который читают чужие руки.
//! Дефекты такого кода не видны на стенде: стенд пишет и читает одним и тем же
//! свежим сервером, где «поле потерялось при сохранении» и «поле не прочиталось»
//! взаимно сокращаются. Поэтому круг «записали -> прочитали» проверяется здесь.
//!
//! Отдельно проверяется ЗАЩИЩЁННОЕ чтение. Читатель писался по образцу
//! достижений: мусорная запись обязана быть ПРОПУЩЕНА, а не уронить загрузку
//! персонажа целиком — иначе один порченый ключ стоит игроку всего аккаунта.
//! Каждая форма мусора проверяется своей строкой, а не «списком вообще».

#include <libserver/data/file/FileDataSource.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

//! ★НЕ `assert`. Образ и юнит-прогон раунда собираются `RelWithDebInfo`, а он
//! несёт `-DNDEBUG` — то есть `assert` выкидывается препроцессором и тест,
//! написанный на нём, ПРОХОДИТ ВСЕГДА. Своя проверка живёт вне зависимости
//! от NDEBUG.
int g_failures = 0;

void Check(const bool condition, const char* const what, const int line)
{
  if (condition)
    return;
  std::fprintf(stderr, "TestCourseRecords.cpp:%d: ПРОВАЛ — %s\n", line, what);
  ++g_failures;
}

#define CHECK(cond, what) Check((cond), (what), __LINE__)

using server::data::Character;

//! Свежий каталог данных на каждый прогон: тест, унаследовавший чужой файл,
//! доказывает не то, что думает.
std::filesystem::path MakeDataDir()
{
  auto dir = std::filesystem::temp_directory_path()
    / std::filesystem::path("loa-r75-course-records");
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

//! Кладёт сырой JSON персонажа мимо писателя — так проверяется именно ЧИТАТЕЛЬ,
//! включая формы, которые наш собственный писатель никогда бы не породил.
void WriteRawCharacter(
  const std::filesystem::path& dataDir,
  const uint32_t uid,
  const nlohmann::json& json)
{
  const auto path = dataDir / "characters" / (std::to_string(uid) + ".json");
  std::ofstream out(path);
  out << json.dump(2);
}

void TestRoundTrip(server::FileDataSource& source)
{
  Character stored;
  stored.uid = 4001;
  stored.name = std::string("roundtrip");
  stored.totalSpeedGames = 7u;
  stored.totalMagicGames = 3u;
  stored.courseRecords = std::vector<Character::CourseRecord>{
    {.courseId = 1, .recordTime = 95000, .timesRaced = 4},
    {.courseId = 2, .recordTime = 120500, .timesRaced = 1}};

  source.StoreCharacter(4001, stored);

  Character loaded;
  source.RetrieveCharacter(4001, loaded);

  CHECK(loaded.totalSpeedGames() == 7u, "totalSpeedGames пережил круг");
  CHECK(loaded.totalMagicGames() == 3u, "totalMagicGames пережил круг");
  CHECK(loaded.courseRecords().size() == 2, "оба рекорда пережили круг");
  if (loaded.courseRecords().size() == 2)
  {
    const auto& first = loaded.courseRecords()[0];
    const auto& second = loaded.courseRecords()[1];
    CHECK(first.courseId == 1, "courseId первой записи");
    CHECK(first.recordTime == 95000, "recordTime первой записи");
    CHECK(first.timesRaced == 4, "timesRaced первой записи");
    CHECK(second.courseId == 2, "courseId второй записи");
    CHECK(second.recordTime == 120500, "recordTime второй записи");
    CHECK(second.timesRaced == 1, "timesRaced второй записи");
  }
}

//! Старый файл ключей не имеет. Миграция — нулевая, и это утверждение обязано
//! быть проверено, а не подразумеваться: именно оно решает, переживут ли
//! рестарт уже существующие персонажи.
void TestZeroMigration(
  server::FileDataSource& source,
  const std::filesystem::path& dataDir)
{
  WriteRawCharacter(dataDir, 4002, nlohmann::json{{"uid", 4002}, {"name", "old"}});

  // ★Запись СВЕЖАЯ — ровно так её и заводит боевой путь (DataDirector создаёт
  // объект и отдаёт его читателю). Проверять «читатель затирает остаток чужого
  // объекта» здесь было бы проверкой несуществующего сценария: читатель писан по
  // образцу достижений и отсутствующий ключ намеренно НЕ трогает поле.
  Character loaded;
  source.RetrieveCharacter(4002, loaded);

  CHECK(loaded.courseRecords().empty(),
    "старый файл без ключа courseRecords даёт ПУСТОЙ список");
  CHECK(loaded.totalSpeedGames() == 0u,
    "старый файл без ключа totalSpeedGames даёт ноль");
  CHECK(loaded.totalMagicGames() == 0u,
    "старый файл без ключа totalMagicGames даёт ноль");
  CHECK(loaded.name() == std::string("old"),
    "персонаж со старым файлом ЗАГРУЗИЛСЯ, а не упал на новых ключах");
}

//! Защищённое чтение. Каждая строка массива — своя форма мусора, и все они
//! обязаны быть пропущены поимённо, а сам персонаж — загрузиться.
void TestDefensiveRead(
  server::FileDataSource& source,
  const std::filesystem::path& dataDir)
{
  nlohmann::json records = nlohmann::json::array();
  records.push_back(42);                                   // не объект
  records.push_back({{"recordTime", 10}});                 // нет courseId
  records.push_back({{"courseId", 0}});                    // courseId == 0
  records.push_back({{"courseId", 70000}});                // не влезает в uint16
  records.push_back({{"courseId", -3}});                   // отрицательный
  records.push_back({{"courseId", "1"}});                  // строка
  records.push_back({{"courseId", 1.5}});                  // дробный
  records.push_back({{"courseId", 5},                      // ГОДНАЯ
    {"recordTime", 88000}, {"timesRaced", 2}});
  records.push_back({{"courseId", 5},                      // дубль по courseId
    {"recordTime", 1}, {"timesRaced", 999}});
  records.push_back({{"courseId", 6},                      // годная, поля мусорные
    {"recordTime", "nope"}, {"timesRaced", -1}});

  WriteRawCharacter(dataDir, 4003,
    nlohmann::json{{"uid", 4003}, {"name", "junk"}, {"courseRecords", records}});

  Character loaded;
  source.RetrieveCharacter(4003, loaded);

  CHECK(loaded.courseRecords().size() == 2,
    "из десяти строк приняты ровно две годные, остальные пропущены");
  if (loaded.courseRecords().size() == 2)
  {
    const auto& first = loaded.courseRecords()[0];
    CHECK(first.courseId == 5, "принята годная запись courseId 5");
    CHECK(first.recordTime == 88000, "её recordTime прочитан");
    CHECK(first.timesRaced == 2,
      "её timesRaced прочитан, и ДУБЛЬ по courseId (timesRaced 999) его не перебил");
    CHECK(loaded.courseRecords()[1].courseId == 6,
      "принята вторая годная запись");
    CHECK(loaded.courseRecords()[1].recordTime == 0,
      "мусорный recordTime не прочитан, поле осталось нулём");
    CHECK(loaded.courseRecords()[1].timesRaced == 0,
      "мусорный timesRaced не прочитан, поле осталось нулём");
  }
}

//! Кап списка. Провод пишет длину одним байтом, поэтому файл длиннее 255
//! записей обязан быть срезан НА ЧТЕНИИ — иначе кадр не соберётся.
void TestCap(
  server::FileDataSource& source,
  const std::filesystem::path& dataDir)
{
  nlohmann::json records = nlohmann::json::array();
  for (uint32_t i = 1; i <= 400; ++i)
    records.push_back({{"courseId", i}, {"recordTime", i}, {"timesRaced", 1}});

  WriteRawCharacter(dataDir, 4004,
    nlohmann::json{{"uid", 4004}, {"name", "cap"}, {"courseRecords", records}});

  Character loaded;
  source.RetrieveCharacter(4004, loaded);

  CHECK(loaded.courseRecords().size() == server::data::MaxCourseRecords,
    "список срезан капом MaxCourseRecords, а не прочитан целиком");
}

} // namespace

int main()
{
  const auto dataDir = MakeDataDir();

  server::FileDataSource source;
  source.Initialize(dataDir);

  TestRoundTrip(source);
  TestZeroMigration(source, dataDir);
  TestDefensiveRead(source, dataDir);
  TestCap(source, dataDir);

  std::filesystem::remove_all(dataDir);

  if (g_failures != 0)
  {
    std::fprintf(stderr, "=== ПРОВАЛЕНО проверок: %d ===\n", g_failures);
    return EXIT_FAILURE;
  }
  std::fprintf(stderr, "=== TestCourseRecords: все проверки пройдены ===\n");
  return EXIT_SUCCESS;
}
