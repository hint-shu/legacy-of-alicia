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

#include "libserver/registry/BreedingOdds.hpp"

namespace server::registry
{

std::optional<std::size_t> CoatTierToOddsIndex(const Coat::Tier tier)
{
  switch (tier)
  {
    case Coat::Tier::Common:   return 0;
    case Coat::Tier::Uncommon: return 1;
    case Coat::Tier::Rare:     return 2;
  }
  // ★NOT `return 0`. A value outside the enum is a bug, not a common coat; the
  // caller must refuse the roll and say so. See the header for the full story.
  return std::nullopt;
}

WeightedChoices BuildEmblemTierChoices(const std::vector<EmblemRatio>& ratios)
{
  WeightedChoices choices;
  int32_t totalRatio = 0;
  for (const auto& ratio : ratios)
  {
    if (ratio.ratio <= 0)
      continue;
    choices.values.push_back(ratio.odds);
    choices.weights.push_back(ratio.ratio);
    totalRatio += ratio.ratio;
  }

  // The shortfall to 100 is the stock "no emblem" share. It is derived from the
  // data, so editing emblemRatios moves it automatically; nothing is hardcoded.
  if (totalRatio > 0 && totalRatio < 100)
  {
    choices.values.push_back(kNoEmblemTier);
    choices.weights.push_back(100 - totalRatio);
  }

  return choices;
}

WeightedChoices BuildPotentialTypeChoices(
  const HorseRegistry& registry,
  const std::size_t tierIndex)
{
  WeightedChoices choices;
  const auto& types = registry.GetPotentialTypes();
  choices.values.reserve(types.size());
  choices.weights.reserve(types.size());

  for (const uint32_t type : types)
  {
    const auto* info = registry.GetPotentialInfo(type);
    if (info == nullptr)
      continue;
    if (tierIndex >= info->oddsByCoatTier.size())
      continue;
    const int32_t weight = info->oddsByCoatTier[tierIndex];
    if (weight <= 0)
      continue;
    choices.values.push_back(type);
    choices.weights.push_back(weight);
  }

  return choices;
}

} // namespace server::registry
