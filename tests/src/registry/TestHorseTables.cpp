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

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace
{

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

//! I1. The rarest coat must not be reachable by breeding.
void TestRarestCoatIsOutOfTheBreedingPool()
{
  const auto registry = LoadHorses();

  BreedingRegistry breeding;
  breeding.ReadConfig(std::filesystem::path(LOA_CONFIG_GAME_DIR) / "breeding.yaml");
  const auto childGradeLimit = breeding.GetBreedingParams().childGradeLimit;

  // Stock DNA_SkinInfo ShapeID 20 (Dapple Bay) has LimitGrade 10.
  const auto& dappleBay = registry.GetCoatInfo(20);
  assert(dappleBay.tid == 20);
  assert(dappleBay.minGrade == 10);

  // A foal can never exceed childGradeLimit, so minGrade above it means the coat
  // cannot be rolled for a foal at all. That is the whole point of the fix.
  assert(dappleBay.minGrade > childGradeLimit);
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
    assert(it != kStockManes.end());
    assert(NearlyEqual(mane.inheritanceRate, it->second.inheritanceRate));
    assert(mane.minGrade == it->second.minGrade);
    assert(mane.tier == it->second.tier);
    ++checkedManes;
  }
  // Floor against a silently empty scan: a check that read nothing says "clean".
  assert(checkedManes == 40);

  int checkedTails = 0;
  for (Tid tid = 1; tid <= 30; ++tid)
  {
    const auto& tail = registry.GetTail(tid);
    if (tail.tid != tid)
      continue;
    const auto it = kStockTails.find(tail.shape);
    assert(it != kStockTails.end());
    assert(NearlyEqual(tail.inheritanceRate, it->second.inheritanceRate));
    assert(tail.minGrade == it->second.minGrade);
    assert(tail.tier == it->second.tier);
    ++checkedTails;
  }
  assert(checkedTails == 30);
}

//! I3. Fertility of grades 6/7/8 equals stock.
void TestFertilityMatchesStock()
{
  const auto registry = LoadHorses();

  const std::map<uint32_t, int32_t> expected = {{6, 70}, {7, 63}, {8, 56}};
  for (const auto& [grade, value] : expected)
  {
    const auto* info = registry.GetGradeInfo(grade);
    assert(info != nullptr);
    assert(info->pregnantValue == value);
  }

  // Untouched neighbours, so a sweep that overshot is visible here.
  const auto* grade5 = registry.GetGradeInfo(5);
  assert(grade5 != nullptr && grade5->pregnantValue == 77);
  const auto* grade9 = registry.GetGradeInfo(9);
  assert(grade9 != nullptr && grade9->pregnantValue == 56);
}

//! M6 data. Every potential type carries three coat-tier weights.
void TestPotentialOddsArePresentAndBanded()
{
  const auto registry = LoadHorses();

  const auto& types = registry.GetPotentialTypes();
  assert(types.size() == 14);

  int32_t columnSums[3] = {0, 0, 0};
  for (const uint32_t type : types)
  {
    const auto* info = registry.GetPotentialInfo(type);
    assert(info != nullptr);
    for (std::size_t i = 0; i < 3; ++i)
    {
      // Every weight must be a real positive number; a zero column would put the
      // type roll back on the uniform coin flip this round removed.
      assert(info->oddsByCoatTier[i] > 0);
      columnSums[i] += info->oddsByCoatTier[i];
    }
  }

  // Band layout of potential.yaml: 4 weak, 5 medium, 5 strong.
  assert(columnSums[0] == 430);
  assert(columnSums[1] == 470);
  assert(columnSums[2] == 490);
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
    assert(stripped);
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
  assert(threw);
}

} // namespace

int main()
{
  TestRarestCoatIsOutOfTheBreedingPool();
  TestManeAndTailRowsMatchTheirShape();
  TestFertilityMatchesStock();
  TestPotentialOddsArePresentAndBanded();
  TestMissingOddsKeyFailsTheLoad();
  std::printf("registry_test_horse_tables: all checks passed\n");
  return 0;
}
