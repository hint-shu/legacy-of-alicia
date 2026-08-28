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

#include "libserver/registry/CareSkillRegistry.hpp"
#include "libserver/util/QuietLog.hpp"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>
#include <string>

namespace server::registry
{

namespace
{

CareEffectOp ParseOp(const std::string& op)
{
  if (op == "add")
    return CareEffectOp::Add;
  if (op == "sub")
    return CareEffectOp::Sub;
  if (op == "mul")
    return CareEffectOp::Mul;
  if (op == "abs")
    return CareEffectOp::Abs;
  return CareEffectOp::None;
}

} // anon namespace

void CareSkillRegistry::ReadConfig(const std::filesystem::path& configPath)
{
  const auto root = YAML::LoadFile(configPath.string());
  const auto careSkills = root["careSkills"];

  // Levels: cumulative XP thresholds for the care ("Смотритель") class.
  for (const auto& levelNode : careSkills["levels"])
  {
    const auto level = static_cast<uint8_t>(levelNode["level"].as<uint32_t>());
    const auto exp = levelNode["exp"].as<uint32_t>();
    _levels.emplace_back(level, exp);
  }
  std::sort(
    _levels.begin(),
    _levels.end(),
    [](const auto& a, const auto& b)
    {
      return a.second < b.second;
    });

  // Ranks: keyed by (skillId, rank); also track the max rank per skill.
  for (const auto& rankNode : careSkills["ranks"])
  {
    CareRankInfo info;
    info.skillId = static_cast<uint8_t>(rankNode["id"].as<uint32_t>());
    info.rank = static_cast<uint8_t>(rankNode["rank"].as<uint32_t>());
    info.levelReq = static_cast<uint8_t>(rankNode["level"].as<uint32_t>());
    info.pointCost = static_cast<uint8_t>(rankNode["point"].as<uint32_t>());
    info.preSkillId = static_cast<uint8_t>(rankNode["preId"].as<uint32_t>(0));
    info.preSkillRank = static_cast<uint8_t>(rankNode["preRank"].as<uint32_t>(0));
    info.effectType = static_cast<uint8_t>(rankNode["effectType"].as<uint32_t>(0));
    info.op = ParseOp(rankNode["op"].as<std::string>("none"));
    info.value1 = rankNode["v1"].as<float>(0.0f);
    info.value2 = rankNode["v2"].as<float>(0.0f);

    _ranks.emplace(std::make_pair(info.skillId, info.rank), info);
    auto& maxRank = _maxRank[info.skillId];
    if (info.rank > maxRank)
      maxRank = info.rank;
  }

  server::util::QuietLogInfo(
    "Care-skill registry loaded {} skills, {} levels, {} ranks",
    _maxRank.size(),
    _levels.size(),
    _ranks.size());
}

const CareRankInfo* CareSkillRegistry::GetRankInfo(uint8_t skillId, uint8_t rank) const
{
  const auto it = _ranks.find(std::make_pair(skillId, rank));
  if (it == _ranks.end())
    return nullptr;
  return &it->second;
}

uint8_t CareSkillRegistry::GetMaxRank(uint8_t skillId) const
{
  const auto it = _maxRank.find(skillId);
  if (it == _maxRank.end())
    return 0;
  return it->second;
}

uint8_t CareSkillRegistry::GetLevelForProgress(uint32_t progress) const
{
  // _levels is sorted by exp ascending; take the highest level whose threshold
  // is <= progress. Minimum 1; saturates at the top level.
  uint8_t level = 1;
  for (const auto& [lvl, exp] : _levels)
  {
    if (exp <= progress)
      level = lvl;
    else
      break;
  }
  return level;
}

uint32_t CareSkillRegistry::SumAddBonus(
  const std::vector<std::pair<uint8_t, uint8_t>>& learned,
  uint8_t effectType) const
{
  uint32_t sum = 0;
  for (const auto& [skillId, rank] : learned)
  {
    const auto* info = GetRankInfo(skillId, rank);
    if (info != nullptr
      && info->effectType == effectType
      && info->op == CareEffectOp::Add)
    {
      sum += static_cast<uint32_t>(info->value1);
    }
  }
  return sum;
}

} // namespace server::registry
