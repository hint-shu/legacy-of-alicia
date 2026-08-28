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

#ifndef CARESKILLREGISTRY_HPP
#define CARESKILLREGISTRY_HPP

#include <cstdint>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace server::registry
{

//! Parsed effect operator (client formula pre-parsed into apply_patches.py yaml).
enum class CareEffectOp : uint8_t
{
  None,
  Add,
  Sub,
  Mul,
  Abs
};

//! One CareSkillRankInfo row, keyed by (skillId, rank).
struct CareRankInfo
{
  uint8_t skillId{};
  uint8_t rank{};
  //! Required care-class level to buy this rank.
  uint8_t levelReq{};
  //! Care-point cost to buy this rank.
  uint8_t pointCost{};
  //! Prerequisite skill id (0 = none) and its required rank.
  uint8_t preSkillId{};
  uint8_t preSkillRank{};
  //! Effect type (0 = plenitude ceiling, 1 = attachment ceiling, ...).
  uint8_t effectType{};
  CareEffectOp op{CareEffectOp::None};
  float value1{};
  float value2{};
};

//! Server-side registry of care-skill tables (skills/levels/ranks), loaded from
//! resources/config/game/care_skills.yaml at startup. Read-only afterwards.
class CareSkillRegistry final
{
public:
  void ReadConfig(const std::filesystem::path& configPath);

  //! (skillId, rank) -> row, or nullptr if that rank does not exist (used for
  //! study validation / rank-up: nullptr means unknown skill or above max rank).
  const CareRankInfo* GetRankInfo(uint8_t skillId, uint8_t rank) const;

  //! Highest existing rank of a skill (0 if the skill is unknown).
  uint8_t GetMaxRank(uint8_t skillId) const;

  //! Care-class level for a cumulative progress value (CareSkillLevel table).
  //! Monotone; minimum 1, saturates at the top level (40 / progress 2675).
  uint8_t GetLevelForProgress(uint32_t progress) const;

  //! Sum of the INTEGER add-bonus over all given (skillId, rank) whose row has
  //! the requested effectType and op==Add. Exactly what the plenitude/attachment
  //! ceilings need (EffectType 0/1). Non-add ops contribute 0.
  uint32_t SumAddBonus(
    const std::vector<std::pair<uint8_t, uint8_t>>& learned,
    uint8_t effectType) const;

private:
  std::map<std::pair<uint8_t, uint8_t>, CareRankInfo> _ranks; // key = (id, rank)
  std::unordered_map<uint8_t, uint8_t> _maxRank;              // id -> max rank
  std::vector<std::pair<uint8_t, uint32_t>> _levels;         // (level, exp) asc
};

} // namespace server::registry

#endif // CARESKILLREGISTRY_HPP
