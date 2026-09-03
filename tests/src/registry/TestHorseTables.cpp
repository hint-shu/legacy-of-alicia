/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2026 Story Of Alicia
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

//! R77. Guards the breeding tables against the stock libconfig_c.dat values.
//! These are TOTAL invariants over every mane and tail row, not a list of the
//! rows this round happened to touch: the defect being closed is "a block got
//! the numbers of the wrong ShapeID", and that class re-appears wherever a new
//! row is added, not only where it was found.

#include "libserver/data/DataDefinitions.hpp"
#include "libserver/registry/BreedingRegistry.hpp"
#include "libserver/registry/HorseRegistry.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <vector>
#include <map>
#include <string>

namespace
{

constexpr const char* kTestFile = "TestHorseTables.cpp";

//! ★НЕ `assert` (Codex finding 4, iteration 1). Образ и юнит-прогон раунда
//! собираются `RelWithDebInfo`, а он несёт `-DNDEBUG` — то есть `assert`
//! выкидывается препроцессором и тест, написанный на нём, ПРОХОДИТ ВСЕГДА.
//! Ровно этот запрет уже написан в `tests/src/util/TestLogThrottle.cpp:34`, и
//! первая версия этого файла его нарушила: 28 `assert`, зелёных по построению.
//! Своя проверка живёт вне зависимости от NDEBUG и валит процесс кодом 1.
void Check(
  const bool condition,
  const char* const what,
  const int line)
{
  if (condition)
    return;

  std::fprintf(
    stderr,
    "%s:%d: ПРОВАЛ — %s\n",
    kTestFile,
    line,
    what);
  std::fflush(stderr);
  std::exit(1);
}

//! Вариадический на случай запятых внутри выражения (`std::accumulate(a, b, 0)`).
#define CHECK(...) Check((__VA_ARGS__), #__VA_ARGS__, __LINE__)


using server::data::Tid;
using server::registry::BreedingRegistry;
using server::registry::Coat;
using server::registry::HorseRegistry;

//! Stock row: InheritanceRate / LimitGrade / Rare, keyed by ShapeID.
struct StockRow
{
  float inheritanceRate;
  int32_t minGrade;
  int32_t tier;
};

//! DNA_ManeInfo.tsv, all eight shapes.
const std::map<int32_t, StockRow> kStockManes = {
  {1, {30.0f, 1, 1}},
  {2, {20.0f, 6, 2}},
  {3, {30.0f, 1, 1}},
  {4, {30.0f, 1, 1}},
  {5, {30.0f, 1, 1}},
  {6, {30.0f, 1, 1}},
  {7, {15.0f, 4, 2}},
  {8, {5.0f, 7, 2}},
};

//! DNA_TailInfo.tsv, all six shapes.
const std::map<int32_t, StockRow> kStockTails = {
  {1, {30.0f, 1, 1}},
  {2, {30.0f, 1, 1}},
  {3, {20.0f, 7, 2}},
  {4, {30.0f, 1, 1}},
  {5, {30.0f, 1, 1}},
  {6, {30.0f, 1, 1}},
};

bool NearlyEqual(const float a, const float b)
{
  const float diff = a > b ? a - b : b - a;
  return diff < 0.001f;
}

std::filesystem::path HorsesConfigDir()
{
  return std::filesystem::path(LOA_CONFIG_GAME_DIR) / "horses";
}

HorseRegistry LoadHorses()
{
  HorseRegistry registry;
  registry.ReadConfig(HorsesConfigDir());
  return registry;
}

//! Copies the shipped horse config into a scratch directory and rewrites one of
//! its files line by line through `mutate`. Returns the scratch directory.
//! Every negative-config test below goes through here, so none of them can
//! accidentally test a hand-written fixture instead of the shipped tables.
std::filesystem::path MakeBrokenConfig(
  const std::string& fileName,
  const std::function<bool(std::vector<std::string>&)>& mutate,
  const std::string& tag)
{
  const auto temp = std::filesystem::temp_directory_path() / ("loa_r77_" + tag);
  std::filesystem::remove_all(temp);
  std::filesystem::copy(
    HorsesConfigDir(), temp, std::filesystem::copy_options::recursive);

  std::vector<std::string> lines;
  {
    std::ifstream in(temp / fileName);
    std::string line;
    while (std::getline(in, line))
      lines.push_back(line);
  }
  // The mutation must report that it actually changed something: a test whose
  // fixture silently stayed valid would go green on the wrong evidence.
  CHECK(mutate(lines));
  {
    std::ofstream out(temp / fileName, std::ios::trunc);
    for (const auto& line : lines)
      out << line << '\n';
  }
  return temp;
}

//! @returns true if `registry.ReadConfig(path)` threw.
bool ReadConfigThrows(HorseRegistry& registry, const std::filesystem::path& path)
{
  try
  {
    registry.ReadConfig(path);
  }
  catch (const std::exception&)
  {
    return true;
  }
  return false;
}

//! I1. The rarest coat must not be reachable by breeding.
void TestRarestCoatIsOutOfTheBreedingPool()
{
  const auto registry = LoadHorses();

  BreedingRegistry breeding;
  breeding.ReadConfig(std::filesystem::path(LOA_CONFIG_GAME_DIR) / "breeding.yaml");
  const auto childGradeLimit = breeding.GetBreedingParams().childGradeLimit;

  // Stock DNA_SkinInfo ShapeID 20 (Dapple Bay) has LimitGrade 10.
  const auto& dappleBay = registry.GetCoatInfo(20);
  CHECK(dappleBay.tid == 20);
  CHECK(dappleBay.minGrade == 10);

  // A foal can never exceed childGradeLimit, so minGrade above it means the coat
  // cannot be rolled for a foal at all. That is the whole point of the fix.
  CHECK(dappleBay.minGrade > childGradeLimit);
}

//! I2. Every mane and tail row carries the numbers of its OWN ShapeID.
void TestManeAndTailRowsMatchTheirShape()
{
  const auto registry = LoadHorses();

  int checkedManes = 0;
  for (Tid tid = 1; tid <= 40; ++tid)
  {
    const auto& mane = registry.GetMane(tid);
    if (mane.tid != tid)
      continue;
    const auto it = kStockManes.find(mane.shape);
    CHECK(it != kStockManes.end());
    CHECK(NearlyEqual(mane.inheritanceRate, it->second.inheritanceRate));
    CHECK(mane.minGrade == it->second.minGrade);
    CHECK(mane.tier == it->second.tier);
    ++checkedManes;
  }
  // Floor against a silently empty scan: a check that read nothing says "clean".
  CHECK(checkedManes == 40);

  int checkedTails = 0;
  for (Tid tid = 1; tid <= 30; ++tid)
  {
    const auto& tail = registry.GetTail(tid);
    if (tail.tid != tid)
      continue;
    const auto it = kStockTails.find(tail.shape);
    CHECK(it != kStockTails.end());
    CHECK(NearlyEqual(tail.inheritanceRate, it->second.inheritanceRate));
    CHECK(tail.minGrade == it->second.minGrade);
    CHECK(tail.tier == it->second.tier);
    ++checkedTails;
  }
  CHECK(checkedTails == 30);
}

//! I3. Fertility of grades 6/7/8 equals stock.
void TestFertilityMatchesStock()
{
  const auto registry = LoadHorses();

  const std::map<uint32_t, int32_t> expected = {{6, 70}, {7, 63}, {8, 56}};
  for (const auto& [grade, value] : expected)
  {
    const auto* info = registry.GetGradeInfo(grade);
    CHECK(info != nullptr);
    CHECK(info->pregnantValue == value);
  }

  // Untouched neighbours, so a sweep that overshot is visible here.
  const auto* grade5 = registry.GetGradeInfo(5);
  CHECK(grade5 != nullptr && grade5->pregnantValue == 77);
  const auto* grade9 = registry.GetGradeInfo(9);
  CHECK(grade9 != nullptr && grade9->pregnantValue == 56);
}

//! M6 data. Every potential type carries three coat-tier weights.
void TestPotentialOddsArePresentAndBanded()
{
  const auto registry = LoadHorses();

  const auto& types = registry.GetPotentialTypes();
  CHECK(types.size() == 14);

  int32_t columnSums[3] = {0, 0, 0};
  for (const uint32_t type : types)
  {
    const auto* info = registry.GetPotentialInfo(type);
    CHECK(info != nullptr);
    for (std::size_t i = 0; i < 3; ++i)
    {
      // Every weight must be a real positive number; a zero column would put the
      // type roll back on the uniform coin flip this round removed.
      CHECK(info->oddsByCoatTier[i] > 0);
      columnSums[i] += info->oddsByCoatTier[i];
    }
  }

  // Band layout of potential.yaml: 4 weak, 5 medium, 5 strong.
  CHECK(columnSums[0] == 430);
  CHECK(columnSums[1] == 470);
  CHECK(columnSums[2] == 490);
}

//! I5b. A missing oddsRare key must THROW, not read as a silent zero.
//! This is the round's guard against its own false green.
void TestMissingOddsKeyFailsTheLoad()
{
  const auto temp = std::filesystem::temp_directory_path() / "loa_r77_horse_tables";
  std::filesystem::remove_all(temp);
  std::filesystem::copy(
    HorsesConfigDir(), temp, std::filesystem::copy_options::recursive);

  // Strip the first oddsRare1 line from the copy.
  const auto potentialPath = temp / "potential.yaml";
  std::string content;
  {
    std::ifstream in(potentialPath);
    std::string line;
    bool stripped = false;
    while (std::getline(in, line))
    {
      if (not stripped && line.find("oddsRare1:") != std::string::npos)
      {
        stripped = true;
        continue;
      }
      content += line;
      content += '\n';
    }
    CHECK(stripped);
  }
  {
    std::ofstream out(potentialPath, std::ios::trunc);
    out << content;
  }

  bool threw = false;
  try
  {
    HorseRegistry registry;
    registry.ReadConfig(temp);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  std::filesystem::remove_all(temp);
  CHECK(threw);
}

//! ★R77-fix-2, Codex finding 1. A FAILED reload must leave the previous tables
//! intact. Before the fix `ReadConfig` cleared every container and then parsed
//! into it, so a throw halfway through (which R77 itself introduced, by making
//! the oddsRare keys mandatory) left an EMPTY registry behind — and `/reload`
//! catches the exception and keeps the server running on it.
void TestFailedReloadKeepsThePreviousTables()
{
  HorseRegistry registry;
  registry.ReadConfig(HorsesConfigDir());

  // Baseline, read from the live registry before the bad reload.
  CHECK(registry.GetPotentialTypes().size() == 14);
  CHECK(registry.GetCoatInfo(20).minGrade == 10);

  const auto broken = MakeBrokenConfig(
    "potential.yaml",
    [](std::vector<std::string>& lines)
    {
      for (std::size_t i = 0; i < lines.size(); ++i)
      {
        if (lines[i].find("oddsRare1:") != std::string::npos)
        {
          lines.erase(lines.begin() + static_cast<long>(i));
          return true;
        }
      }
      return false;
    },
    "reload_keeps");

  CHECK(ReadConfigThrows(registry, broken));

  // ★THE POINT: the old table still answers, byte for byte.
  CHECK(registry.GetPotentialTypes().size() == 14);
  CHECK(registry.GetPotentialInfo(1) != nullptr);
  CHECK(registry.GetCoatInfo(20).minGrade == 10);
  CHECK(registry.GetGradeInfo(6) != nullptr && registry.GetGradeInfo(6)->pregnantValue == 70);

  std::filesystem::remove_all(broken);
}

//! ★R77-fix-2, Codex finding 1, second lock. `size() - 1` on an empty vector is
//! SIZE_MAX, not -1. A registry that never loaded anything must refuse the admin
//! roll rather than index a vector of length zero.
void TestRandomPotentialRefusesAnEmptyRegistry()
{
  HorseRegistry empty;
  server::data::Horse::Potential potential{};
  potential.type = 42;
  potential.level = 7;
  potential.value = 9;

  empty.GiveHorseRandomPotential(potential);

  // Untouched: the roll refused instead of writing a value read out of bounds.
  CHECK(potential.type() == 42);
  CHECK(potential.level() == 7);
  CHECK(potential.value() == 9);
}

//! ★R77-fix-2, Codex finding 2. A coat star tier outside 1..3 used to be cast
//! straight into the enum and then silently mapped onto the common-coat column.
void TestInvalidCoatTierFailsTheLoad()
{
  const auto broken = MakeBrokenConfig(
    "appearance.yaml",
    [](std::vector<std::string>& lines)
    {
      for (auto& line : lines)
      {
        if (line.find("    tier: ") != std::string::npos)
        {
          line = "    tier: 9";
          return true;
        }
      }
      return false;
    },
    "coat_tier");

  HorseRegistry registry;
  CHECK(ReadConfigThrows(registry, broken));
  std::filesystem::remove_all(broken);
}

//! ★R77-fix-2, Codex finding 3. Bad emblem configuration is a STARTUP error, so
//! the emblem roll never has to invent a tier (1) or an emblem id (1) at runtime.
void TestEmblemConfigIsValidatedAtLoad()
{
  // (a) every ratio zero -> no tier can be rolled at all.
  {
    const auto broken = MakeBrokenConfig(
      "emblems.yaml",
      [](std::vector<std::string>& lines)
      {
        bool changed = false;
        for (auto& line : lines)
        {
          if (line.find("    ratio: ") != std::string::npos)
          {
            line = "    ratio: 0";
            changed = true;
          }
        }
        return changed;
      },
      "emblem_zero");
    HorseRegistry registry;
    CHECK(ReadConfigThrows(registry, broken));
    std::filesystem::remove_all(broken);
  }

  // (b) a rollable tier with no emblems in it -> the roll would have returned 1.
  {
    const auto broken = MakeBrokenConfig(
      "emblems.yaml",
      [](std::vector<std::string>& lines)
      {
        for (auto& line : lines)
        {
          // Only emblemRatios rows are written as a list item at this indent;
          // the emblems themselves carry `odds` under an `id`.
          if (line == "  - odds: 3")
          {
            line = "  - odds: 4";
            return true;
          }
        }
        return false;
      },
      "emblem_uncovered");
    HorseRegistry registry;
    CHECK(ReadConfigThrows(registry, broken));
    std::filesystem::remove_all(broken);
  }

  // (c) ★fix-3 (Codex iteration 2, BLOCK 2): tier 0 is the reserved "no emblem"
  // sentinel. A ratio row on tier 0 would be rolled and then read as "no emblem",
  // so real emblems would silently disappear. It must be refused at load.
  {
    const auto broken = MakeBrokenConfig(
      "emblems.yaml",
      [](std::vector<std::string>& lines)
      {
        for (auto& line : lines)
        {
          if (line == "  - odds: 1")
          {
            line = "  - odds: 0";
            return true;
          }
        }
        return false;
      },
      "emblem_sentinel");
    HorseRegistry registry;
    CHECK(ReadConfigThrows(registry, broken));
    std::filesystem::remove_all(broken);
  }

  // (d) ★fix-3 (Codex iteration 2, WARN 3): ratios are WEIGHTS, and a sum of 100
  // or more is legal by the documented contract of BuildEmblemTierChoices -- it
  // simply means there is no "no emblem" share. The loader must NOT refuse it,
  // otherwise the validation and the helper disagree about what a legal file is.
  {
    const auto legal = MakeBrokenConfig(
      "emblems.yaml",
      [](std::vector<std::string>& lines)
      {
        for (auto& line : lines)
        {
          if (line == "    ratio: 77")
          {
            line = "    ratio: 177";
            return true;
          }
        }
        return false;
      },
      "emblem_over100");
    HorseRegistry registry;
    CHECK(not ReadConfigThrows(registry, legal));
    std::filesystem::remove_all(legal);
  }

  // And the SHIPPED table still loads, so "always throws" is not a pass.
  HorseRegistry registry;
  CHECK(not ReadConfigThrows(registry, HorsesConfigDir()));
}

} // namespace

int main()
{
  TestRarestCoatIsOutOfTheBreedingPool();
  TestManeAndTailRowsMatchTheirShape();
  TestFertilityMatchesStock();
  TestPotentialOddsArePresentAndBanded();
  TestMissingOddsKeyFailsTheLoad();
  TestFailedReloadKeepsThePreviousTables();
  TestRandomPotentialRefusesAnEmptyRegistry();
  TestInvalidCoatTierFailsTheLoad();
  TestEmblemConfigIsValidatedAtLoad();
  std::printf("registry_test_horse_tables: all checks passed\n");
  return 0;
}
