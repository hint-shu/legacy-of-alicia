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

#ifndef BREEDING_ODDS_HPP
#define BREEDING_ODDS_HPP

#include "libserver/registry/HorseRegistry.hpp"

#include <cstdint>
#include <vector>

namespace server::registry
{

//! Sentinel tier standing for "this foal gets no emblem at all".
//! Stock EmblemRatioInfo sums to 92, not 100; the missing 8 is that outcome.
//! Zero is safe to hand downstream: data::Horse::emblemUid already defaults to 0
//! for every horse that was not bred, and the protocol writes it as a plain uint16.
constexpr uint32_t kNoEmblemTier = 0;

//! A parallel value/weight pair, ready for a weighted pick.
struct WeightedChoices
{
  std::vector<uint32_t> values;
  std::vector<int32_t> weights;

  [[nodiscard]] bool empty() const { return values.empty(); }
};

//! Maps a coat star tier onto the OddsRare column index of PotentialInfo.
//! Exhaustive switch with no default on purpose: adding a tier must break the
//! build here rather than silently map to column 0.
[[nodiscard]] std::size_t CoatTierToOddsIndex(Coat::Tier tier);

//! Builds the emblem-tier choices for one roll, from emblems.yaml -> emblemRatios.
//! When the configured ratios sum to less than 100 the shortfall is added as an
//! explicit kNoEmblemTier candidate, so the "no emblem" share is a CONSEQUENCE of
//! the data rather than a hardcoded percentage. When they sum to 100 or more the
//! result is byte-for-byte the old candidate list.
[[nodiscard]] WeightedChoices BuildEmblemTierChoices(
  const std::vector<EmblemRatio>& ratios);

//! Builds the potential-type choices for a foal whose coat has the given star tier.
//! Types with a non-positive weight for that tier are not candidates at all.
//! An empty result means "no potential" -- never "fall back to uniform".
[[nodiscard]] WeightedChoices BuildPotentialTypeChoices(
  const HorseRegistry& registry,
  std::size_t tierIndex);

} // namespace server::registry

#endif // BREEDING_ODDS_HPP
