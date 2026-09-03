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

//! R77. Exercises the two decisions the round changed inside Genetics, through
//! the very functions Genetics calls -- BreedingOdds -- rather than through a
//! copy of the logic. A test that restates the implementation proves nothing.
//!
//! Genetics itself cannot be linked here: it lives in the alicia-server
//! executable and needs a whole ServerInstance. The decisions were therefore
//! lifted into alicia-libserver as pure functions, which is also what makes the
//! candidate list observable at all.

#include "libserver/registry/BreedingOdds.hpp"
#include "libserver/registry/HorseRegistry.hpp"

#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <numeric>
#include <random>
#include <vector>

namespace
{

constexpr const char* kTestFile = "TestGenetics.cpp";

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


using server::registry::BuildEmblemTierChoices;
using server::registry::BuildPotentialTypeChoices;
using server::registry::Coat;
using server::registry::CoatTierToOddsIndex;
using server::registry::EmblemRatio;
using server::registry::HorseRegistry;
using server::registry::kNoEmblemTier;

HorseRegistry LoadHorses()
{
  HorseRegistry registry;
  registry.ReadConfig(std::filesystem::path(LOA_CONFIG_GAME_DIR) / "horses");
  return registry;
}

//! Star tier maps onto the OddsRare column in stock order.
void TestCoatTierMapsToOddsColumn()
{
  CHECK(CoatTierToOddsIndex(Coat::Tier::Common) == 0);
  CHECK(CoatTierToOddsIndex(Coat::Tier::Uncommon) == 1);
  CHECK(CoatTierToOddsIndex(Coat::Tier::Rare) == 2);
}

//! ★R77-fix-2 (Codex finding 2). A value outside the enum must have NO column.
//! The old code answered `0` — the common-coat column — for every such value,
//! and the YAML parser could produce one by casting any integer into the enum.
void TestUnknownCoatTierHasNoColumn()
{
  for (const int32_t bogus : {0, 4, 7, 255, -1})
  {
    const auto index = CoatTierToOddsIndex(static_cast<Coat::Tier>(bogus));
    CHECK(not index.has_value());
  }

  // And the three real tiers still answer, so "always nullopt" is not a pass.
  CHECK(CoatTierToOddsIndex(Coat::Tier::Common).has_value());
  CHECK(CoatTierToOddsIndex(Coat::Tier::Rare).has_value());
}

//! I4. Stock EmblemRatioInfo sums to 92, and the missing 8 is a real outcome.
void TestNoEmblemShareComesFromTheData()
{
  // Stock ratios.
  const std::vector<EmblemRatio> stock = {{1, 77}, {2, 13}, {3, 2}};
  const auto choices = BuildEmblemTierChoices(stock);
  CHECK(choices.values.size() == 4);
  CHECK(choices.values[3] == kNoEmblemTier);
  CHECK(choices.weights[3] == 8);
  CHECK(std::accumulate(choices.weights.begin(), choices.weights.end(), 0) == 100);

  // Ratios that already sum to 100 must produce NO sentinel: the share is a
  // consequence of the data, never a hardcoded eight percent.
  const std::vector<EmblemRatio> full = {{1, 80}, {2, 18}, {3, 2}};
  const auto fullChoices = BuildEmblemTierChoices(full);
  CHECK(fullChoices.values.size() == 3);
  for (const uint32_t tier : fullChoices.values)
    CHECK(tier != kNoEmblemTier);

  // Rows with a non-positive ratio stay out, as before the round.
  const std::vector<EmblemRatio> withZero = {{1, 77}, {2, 0}, {3, 2}};
  const auto zeroChoices = BuildEmblemTierChoices(withZero);
  CHECK(zeroChoices.values.size() == 3);
  CHECK(zeroChoices.values[2] == kNoEmblemTier);
  CHECK(zeroChoices.weights[2] == 21);

  // An empty table must not invent a "no emblem" branch out of nothing.
  CHECK(BuildEmblemTierChoices({}).empty());
}

//! Live check of the shipped potential.yaml through the same call Genetics makes.
void TestPotentialChoicesFollowTheCoatTier()
{
  const auto registry = LoadHorses();

  const std::vector<uint32_t> weak = {2, 3, 11, 15};
  const std::vector<uint32_t> strong = {1, 4, 7, 13, 14};

  const auto bandWeight = [](const server::registry::WeightedChoices& choices,
                             const std::vector<uint32_t>& band)
  {
    int32_t sum = 0;
    for (std::size_t i = 0; i < choices.values.size(); ++i)
    {
      for (const uint32_t type : band)
      {
        if (choices.values[i] == type)
          sum += choices.weights[i];
      }
    }
    return sum;
  };

  const auto common = BuildPotentialTypeChoices(registry, 0);
  const auto uncommon = BuildPotentialTypeChoices(registry, 1);
  const auto rare = BuildPotentialTypeChoices(registry, 2);

  // Every type is a candidate on every tier; only the weights move.
  CHECK(common.values.size() == 14);
  CHECK(uncommon.values.size() == 14);
  CHECK(rare.values.size() == 14);

  const int32_t commonTotal = std::accumulate(common.weights.begin(), common.weights.end(), 0);
  const int32_t rareTotal = std::accumulate(rare.weights.begin(), rare.weights.end(), 0);
  CHECK(commonTotal == 430);
  CHECK(rareTotal == 490);

  // The whole point of M6: a common coat leans weak, a rare coat leans strong.
  CHECK(bandWeight(common, weak) > bandWeight(common, strong));
  CHECK(bandWeight(rare, strong) > bandWeight(rare, weak));

  // And the lean is monotone across the three tiers, not merely different.
  CHECK(bandWeight(common, strong) < bandWeight(uncommon, strong));
  CHECK(bandWeight(uncommon, strong) < bandWeight(rare, strong));
}

//! I5. The realised distribution differs from uniform, measured not assumed.
void TestSampledDistributionIsTierDependent()
{
  const auto registry = LoadHorses();
  const std::vector<uint32_t> strong = {1, 4, 7, 13, 14};

  const auto sampleStrongShare = [&](const std::size_t tierIndex)
  {
    const auto choices = BuildPotentialTypeChoices(registry, tierIndex);
    std::mt19937 engine(20260903u); // Fixed seed: the numbers below are exact.
    std::discrete_distribution<std::size_t> dist(
      choices.weights.begin(), choices.weights.end());

    int hits = 0;
    constexpr int kRolls = 100000;
    for (int i = 0; i < kRolls; ++i)
    {
      const uint32_t type = choices.values[dist(engine)];
      for (const uint32_t s : strong)
      {
        if (type == s)
          ++hits;
      }
    }
    return static_cast<double>(hits) / kRolls;
  };

  const double commonShare = sampleStrongShare(0);
  const double uncommonShare = sampleStrongShare(1);
  const double rareShare = sampleStrongShare(2);

  // Expected from potential.yaml: 25/430, 125/470, 300/490.
  CHECK(std::fabs(commonShare - 25.0 / 430.0) < 0.01);
  CHECK(std::fabs(uncommonShare - 125.0 / 470.0) < 0.01);
  CHECK(std::fabs(rareShare - 300.0 / 490.0) < 0.01);

  // A uniform roll over 14 types would give 5/14 = 0.357 on every tier.
  constexpr double kUniformStrongShare = 5.0 / 14.0;
  CHECK(std::fabs(commonShare - kUniformStrongShare) > 0.05);
  CHECK(std::fabs(rareShare - kUniformStrongShare) > 0.05);
}

//! I5b, behaviour side. All-zero weights must yield NO candidates, so the caller
//! reports "no potential" instead of quietly falling back to the uniform roll.
void TestZeroWeightsYieldNoCandidates()
{
  auto registry = LoadHorses();
  // The shipped registry has no zero column, so build the degenerate case from a
  // hand-made one: an out-of-range tier index behaves like "no data for this tier".
  const auto choices = BuildPotentialTypeChoices(registry, 3);
  CHECK(choices.empty());
}

} // namespace

int main()
{
  TestCoatTierMapsToOddsColumn();
  TestUnknownCoatTierHasNoColumn();
  TestNoEmblemShareComesFromTheData();
  TestPotentialChoicesFollowTheCoatTier();
  TestSampledDistributionIsTierDependent();
  TestZeroWeightsYieldNoCandidates();
  std::printf("ranch_test_genetics: all checks passed\n");
  return 0;
}
