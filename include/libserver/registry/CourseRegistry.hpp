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

#ifndef COURSEREGISTRY_HPP
#define COURSEREGISTRY_HPP

#include "libserver/network/command/proto/CommonStructureDefinitions.hpp"
#include "libserver/registry/RegistryDefinitions.hpp"

#include <array>
#include <cstdint>

#include <unordered_map>
#include <filesystem>
#include <vector>

namespace server::registry
{

using GameModeId = uint32_t;
using TeamModeId = uint32_t;

using MapBlockId = uint32_t;
using DeckId = uint32_t;
using DeckItemId = uint32_t;

struct Course
{
  struct GameModeInfo
  {
    enum class Type : uint8_t
    {
      Nothing = 0,
      Speed = 1,
      Magic = 2,
      Guild = 3,
      Tutorial = 6
    };

    //! An amount of star points awarded for a good jump.
    uint32_t goodJumpStarPoints{};
    //! An amount of star points awarded for a perfect jump.
    uint32_t perfectJumpStarPoints{};
    //! A perfect jump max bonus combo.
    uint32_t perfectJumpMaxBonusCombo{};
    //! i dont know
    uint32_t perfectJumpUnitStarPoints{};
    //! i dont know
    uint32_t perfectSpurCheckTime{};
    //! An amount of points a spur consumes.
    uint32_t spurConsumeStarPoints{};
    //! A maximum amount of star points collectable.
    uint32_t starPointsMax{};
    //! A list of used item decks.
    std::vector<DeckId> usedDeckIds{};
    //! A map pool
    std::vector<MapBlockId> mapBlockPool{};
  };

  struct EventInfo
  {
  };

  struct MapBlockInfo
  {
    //! A region this map belongs to.
    Region region{Region::Unknown};
    //! A required level to play the map.
    uint32_t requiredLevel{};
    //! A podium ID.
    uint32_t podiumId{};
    //! An offset to apply to all item deck positions.
    protocol::Vector3 offset{};
    //! A fee for training on the map.
    uint32_t trainingFee{};
    //! A map time limit in seconds.
    uint32_t timeLimit{};
    //! A wait time in seconds.
    uint32_t waitTime{};

    struct DeckItemInstance
    {
      //! An item deck ID.
      DeckId deckId;
      //! A position of the item deck.
      protocol::Vector3 position;
    };

    //! A collection of item deck instances.
    std::vector<DeckItemInstance> itemDecks;
  };

  struct DeckInfo
  {
    //! A regeneration time for items in this deck.
    std::chrono::milliseconds respawnTime{};
    //! A list of items spawned in this deck.
    std::vector<DeckItemId> items;
  };

  struct ItemTypeInfo
  {
    // LOA-fix (R63-2, round63, backlog #174): последнее поле реестров без
    // инициализатора. Пересчёт по дереву: 241 поле с инициализатором, это —
    // единственное без. Незанулённая память реестра уже давала у нас реальные
    // дефекты (13-летняя инкубация от `hatchDuration`, бесплатный инкубатор от
    // `incubatorSlots`), поэтому свойство «у КАЖДОГО поля есть значение»
    // закрывается целиком, а не по мере проявления симптомов.
    uint32_t magicSlot{0};
  };

  //! LOA-fix (R68, backlog #5/#99): ДЕКА СПАВНА КВЕСТОВЫХ ПРЕДМЕТОВ.
  //!
  //! Перенос клиентской таблицы `QuestItemDeckInfo` на сервер. Одна запись
  //! отвечает на вопрос «какой квестовый предмет, на каких картах и в каком
  //! количестве раскладывать гонщику, несущему соответствующий квест».
  //!
  //! ★ЧЕМ ОТЛИЧАЕТСЯ ОТ `DeckInfo`. Обычная дека — ОБЩИЙ для всех гонщиков
  //! спавнер с респавном; квестовая — ПЕР-ГОНЩИКОВЫЙ набор предметов, зависящий
  //! от того, какие квесты игрок сейчас несёт. На одной и той же точке карты у
  //! двух игроков лежит РАЗНОЕ: дека 1002 на карте 1 обслуживает сразу два
  //! квестовых предмета (QTemID 5 и 6), различить их общим спавнером нельзя
  //! в принципе. Поэтому квестовые деки не попадают ни в
  //! `GameModeInfo::usedDeckIds`, ни в `PrepareItemDecks`.
  struct QuestItemDeck
  {
    //! Идентификатор деки (`QDeckID` клиентской таблицы).
    uint32_t questDeckId{};
    //! Идентификатор квестового предмета (`QTemID` клиентской таблицы).
    //! ★ЭТО ЖЕ ЧИСЛО стоит в `Quest::functionValue` у квестов класса
    //! `CollectDropItem` — сверено по всем 17 таким квестам в quests.yaml.
    uint32_t questItemId{};
    //! Сколько предметов выдаётся ОДНОМУ гонщику за ОДИН заезд (`SpawnCnt`).
    uint32_t spawnCount{};

    //! Точка спавна: карта + обычная дека, чьи координаты на этой карте
    //! используются как места раскладки. Клиентский формат — «карта(дека)».
    struct SpawnPoint
    {
      //! Карта, на которой действует эта точка.
      MapBlockId mapBlockId{};
      //! Дека, чьи координаты на карте используются. Это же число уходит
      //! клиенту как `AcCmdRCCreateItem::itemType` и выбирает внешний вид
      //! предмета (клиентская таблица `DeckItemParam`).
      DeckId deckId{};
    };

    //! Точки спавна деки.
    std::vector<SpawnPoint> spawnPoints;
  };

};

class CourseRegistry
{
public:
  CourseRegistry();

  void ReadConfig(const std::filesystem::path& configPath);

  [[nodiscard]] const Course::GameModeInfo& GetCourseGameModeInfo(
    GameModeId type) const;
  [[nodiscard]] const Course::MapBlockInfo& GetMapBlockInfo(
    MapBlockId id) const;
  [[nodiscard]] const Course::DeckInfo& GetDeckInfo(
    DeckId deckId) const;
  [[nodiscard]] const Course::ItemTypeInfo& GetDeckItemInfo(
    DeckItemId itemTypeId) const;
  //! LOA-fix (R68, backlog #5/#99): все деки спавна квестовых предметов.
  //! ★Вектор, а не карта: единственный режим доступа — полный перебор с
  //! отбором по `questItemId` и карте, ключ ни разу не нужен. Порядок
  //! обхода = порядок в конфиге, то есть воспроизводимый.
  [[nodiscard]] const std::vector<Course::QuestItemDeck>& GetQuestItemDecks() const;

private:
  //! A collection of game mode infos.
  std::unordered_map<GameModeId, Course::GameModeInfo> _gameModeInfo;
  //! A collection of map block infos.
  std::unordered_map<MapBlockId, Course::MapBlockInfo> _mapBlockInfo;
  //! A collection of item deck infos.
  std::unordered_map<DeckId, Course::DeckInfo> _itemDeckInfo;
  //! A collection of deck item infos.
  std::unordered_map<DeckItemId, Course::ItemTypeInfo> _deckItemInfo;
  //! LOA-fix (R68, backlog #5/#99): деки спавна квестовых предметов.
  std::vector<Course::QuestItemDeck> _questItemDecks;
 };

}

#endif //COURSEREGISTRY_HPP
