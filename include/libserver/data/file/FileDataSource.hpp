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

#ifndef FILEDATASOURCE_HPP
#define FILEDATASOURCE_HPP

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/data/DataSource.hpp>

#include <atomic>
#include <cstddef>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace server
{

class FileDataSource
  : public DataSource
{
public:
  ~FileDataSource() override = default;

  void Initialize(const std::filesystem::path& path);
  void Terminate();

  void SaveMetadata();

  void CreateUser(data::User& user) override;
  void RetrieveUser(const std::string_view& name, data::User& user) override;
  void StoreUser(const std::string_view& name, const data::User& user) override;
  bool IsUserNameUnique(const std::string_view& name) override;

  void CreateInfraction(data::Infraction& infraction) override;
  void RetrieveInfraction(data::Uid uid, data::Infraction& infraction) override;
  void StoreInfraction(data::Uid uid, const data::Infraction& infraction) override;
  void DeleteInfraction(data::Uid uid) override;

  void CreateCharacter(data::Character& character) override;
  void RetrieveCharacter(data::Uid uid, data::Character& character) override;
  void StoreCharacter(data::Uid uid, const data::Character& character) override;
  void DeleteCharacter(data::Uid uid) override;
  data::Uid RetrieveCharacterUidByName(const std::string_view& name) override;
  bool IsCharacterNameUnique(const std::string_view& name) override;

  void CreateHorse(data::Horse& horse) override;
  void RetrieveHorse(data::Uid uid, data::Horse& horse) override;
  void StoreHorse(data::Uid uid, const data::Horse& horse) override;
  void DeleteHorse(data::Uid uid) override;

  void CreateItem(data::Item& item) override;
  void RetrieveItem(data::Uid uid, data::Item& item) override;
  void StoreItem(data::Uid uid, const data::Item& item) override;
  void DeleteItem(data::Uid uid) override;

  void CreateStorageItem(data::StorageItem& storageItem) override;
  void RetrieveStorageItem(data::Uid uid, data::StorageItem& storageItem) override;
  void StoreStorageItem(data::Uid uid, const data::StorageItem& storageItem) override;
  void DeleteStorageItem(data::Uid uid) override;

  void CreateEgg(data::Egg& egg) override;
  void RetrieveEgg(data::Uid uid, data::Egg& egg) override;
  void StoreEgg(data::Uid uid, const data::Egg& egg) override;
  void DeleteEgg(data::Uid uid) override;

  void CreatePet(data::Pet& pet) override;
  void RetrievePet(data::Uid uid, data::Pet& pet) override;
  void StorePet(data::Uid uid, const data::Pet& pet) override;
  void DeletePet(data::Uid uid) override;

  void CreateHousing(data::Housing& housing) override;
  void RetrieveHousing(data::Uid uid, data::Housing& housing) override;
  void StoreHousing(data::Uid uid, const data::Housing& housing) override;
  void DeleteHousing(data::Uid uid) override;

  void CreateGuild(data::Guild& guild) override;
  void RetrieveGuild(data::Uid uid, data::Guild& guild) override;
  void StoreGuild(data::Uid uid, const data::Guild& guild) override;
  void DeleteGuild(data::Uid uid) override;
  bool IsGuildNameUnique(const std::string_view& name) override;

  void CreateSettings(data::Settings& settings) override;
  void RetrieveSettings(data::Uid uid, data::Settings& settings) override;
  void StoreSettings(data::Uid uid, const data::Settings& settings) override;
  void DeleteSettings(data::Uid uid) override;

  void CreateDailyQuestGroup(data::DailyQuestGroup& group) override;
  void RetrieveDailyQuestGroup(data::Uid uid, data::DailyQuestGroup& group) override;
  void StoreDailyQuestGroup(data::Uid uid, const data::DailyQuestGroup& group) override;
  void DeleteDailyQuestGroup(data::Uid uid) override;

  void CreateMail(data::Mail& mail) override;
  void RetrieveMail(data::Uid uid, data::Mail& mail) override;
  void StoreMail(data::Uid uid, const data::Mail& mail) override;
  void DeleteMail(data::Uid uid) override;

  void CreateQuest(data::Quest& quest) override;
  void RetrieveQuest(data::Uid uid, data::Quest& quest) override;
  void StoreQuest(data::Uid uid, const data::Quest& quest) override;
  void DeleteQuest(data::Uid uid) override;

  void CreateStallion(data::Stallion& stallion) override;
  void RetrieveStallion(data::Uid uid, data::Stallion& stallion) override;
  void StoreStallion(data::Uid uid, const data::Stallion& stallion) override;
  void DeleteStallion(data::Uid uid) override;
  std::vector<data::Uid> ListRegisteredStallions() override;

  void CreateReward(data::Reward& reward) override;
  void RetrieveReward(data::Uid claimUid, data::Reward& reward) override;
  void StoreReward(data::Uid claimUid, const data::Reward& reward) override;
  void DeleteReward(data::Uid claimUid) override;

private:
  //! A root data path.
  std::filesystem::path _dataPath;

  //! A path to the user data files.
  std::filesystem::path _userDataPath;
  //! A path to the infraction data files.
  std::filesystem::path _infractionDataPath;
  //! A path to the character data files.
  std::filesystem::path _characterDataPath;
  //! A path to the horse data files.
  std::filesystem::path _horseDataPath;
  //! A path to the item data files.
  std::filesystem::path _itemDataPath;
  //! A path to the egg data files.
  std::filesystem::path _eggDataPath;
  //! A path to the stored item data files.
  std::filesystem::path _storageItemPath;
  //! A path to the pet data files.
  std::filesystem::path _petDataPath;
  //! A path to the housing data files.
  std::filesystem::path _housingDataPath;
  //! A path to the guild data files.
  std::filesystem::path _guildDataPath;
  //! A path to the settings data files.
  std::filesystem::path _settingsDataPath;
  //! A path to the daily quest group data files.
  std::filesystem::path _dailyQuestGroupDataPath;
  //! A path to the mail data files.
  std::filesystem::path _mailDataPath;
  //! A path to the quest data files.
  std::filesystem::path _questDataPath;
  //! A path to the stallion data files.
  std::filesystem::path _stallionDataPath;
  //! A path to the reward data files.
  std::filesystem::path _rewardDataPath;

  //! A path to meta-data file.
  std::filesystem::path _metaFilePath;

  //! Sequential UID for infractions.
  std::atomic_uint32_t _infractionSequentialUid = 0;
  //! Sequential UID for characters.
  std::atomic_uint32_t _characterSequentialUid = 0;
  //! Sequential UID pool for equipment.
  //! Equipment includes items and horses.
  std::atomic_uint32_t _equipmentSequentialUid = 0;
  //! Sequential UID for storage items.
  std::atomic_uint32_t _storageItemSequentialUid = 0;
  //! Sequential UID for eggs.
  std::atomic_uint32_t _eggSequentialUid = 0;
  //! Sequential UID for pets.
  std::atomic_uint32_t _petSequentialUid = 0;
  //! Sequential UID for housing.
  std::atomic_uint32_t _housingSequentialUid = 0;
  //! Sequential UID for guilds.
  std::atomic_uint32_t _guildSequentialId = 0;
  //! Sequential UID for settings.
  std::atomic_uint32_t _settingsSequentialId = 0;
  //! Sequential UID for daily quest groups.
  std::atomic_uint32_t _dailyQuestGroupSequentialId = 0;
  //! Sequential UID for mail.
  std::atomic_uint32_t _mailSequentialId = 0;
  //! Sequential UID for quests.
  std::atomic_uint32_t _questSequentialId = 0;
  //! Sequential UID for stallions.
  std::atomic_uint32_t _stallionSequentialUid = 0;
  //! Sequential UID for rewards.
  std::atomic_uint32_t _rewardSequentialUid = 0;

  //! ИНДЕКС «имя персонажа (нижний ASCII-регистр) -> ВСЕ uid с этим именем»
  //! (LOA-fix R73-4, #130-C8). Строится один раз на `Initialize`,
  //! поддерживается на `StoreCharacter` и `DeleteCharacter`. Заменяет обход
  //! ВСЕХ файлов персонажей на КАЖДЫЙ поиск.
  //!
  //! ★ХРАНИТСЯ ВЕСЬ СПИСОК, А НЕ ОДИН ПОБЕДИТЕЛЬ, и это правка ревью (итерация
  //! 1). Прежняя редакция при столкновении имён ВЫБРАСЫВАЛА проигравшие uid;
  //! после переименования или удаления победителя имя переставало разрешаться
  //! вовсе — до перезапуска, хотя второй персонаж с этим именем на диске никуда
  //! не девался. Вектор отсортирован по возрастанию, поэтому «меньший uid —
  //! старшая запись» держится и на старте, и в рантайме, а снятие победителя
  //! ДЕТЕРМИНИРОВАННО поднимает следующего.
  std::unordered_map<std::string, std::vector<data::Uid>> _characterNameToUid;
  //! Обратный индекс uid -> КЛЮЧ (не отображаемое имя). Без него ПЕРЕИМЕНОВАНИЕ
  //! оставило бы старое имя занятым навсегда — то есть индекс завёл бы дефект,
  //! которого без него нет.
  std::unordered_map<data::Uid, std::string> _characterUidToName;
  //! Индекс читается сетевыми потоками (лобби/ранчо/мессенджер/гонка) и пишется
  //! потоком тика DataDirector'а через `StoreCharacter`.
  mutable std::shared_mutex _characterNameIndexMutex;

  //! ИНДЕКС СУЩЕСТВОВАНИЯ ИМЁН АККАУНТОВ (LOA-fix R73-4b, #130-C8). Только
  //! ключи: `IsUserNameUnique` спрашивает «есть ли такой», а не «кто это».
  //!
  //! ★ЗАЧЕМ, ЕСЛИ ЕСТЬ ГЕЙТ ИМЕНИ. Гейт снял РЕГУЛЯРКУ, но не снял ОБХОД:
  //! staff-команда с любым именем из класса `[A-Za-z0-9_-]` всё ещё открывала
  //! каталог `data/users` целиком, то есть стоимость пакета росла как O(число
  //! аккаунтов). Найдено ревью (итерация 1). Класс закрывается тем же способом,
  //! что и на стороне персонажей, — индексом, а не вторым частным случаем.
  std::unordered_set<std::string> _userNameKeys;
  mutable std::shared_mutex _userNameIndexMutex;

  //! Потолки длины имени для СТРУКТУРНОГО ГЕЙТА, поднятые до самой длинной
  //! записи, которая реально лежит в индексе (правка ревью, итерация 1).
  //! Ниже `kMaxStoredNameBytes`/`kMaxLoginNameBytes` не опускаются никогда:
  //! это пол, а не значение.
  //! ★ЛИТЕРАЛЫ, А НЕ `kMaxStoredNameBytes`/`kMaxLoginNameBytes`: этот заголовок
  //! намеренно не тянет `NameGuard.hpp` (инлайн-бюджет TU — см. лесенку).
  //! Расхождение ловится `static_assert` в `FileDataSource.cpp`, то есть
  //! КОМПИЛЯЦИЕЙ, а не внимательностью.
  std::atomic_size_t _characterNameCeiling{64};
  std::atomic_size_t _loginNameCeiling{48};

  //! Сколько обращений отбито структурным гейтом имени. Читается РОВНО ОДИН раз,
  //! на `Terminate` — счётчик существует затем, чтобы гейт был проверкой, чей
  //! вердикт кто-то читает, и при этом не давал строку на пакет.
  std::atomic_uint64_t _rejectedNameLookups{0};

  //! Перестроить индекс имён с диска. Зовётся только из `Initialize`.
  void RebuildCharacterNameIndex();
  //! Внести/переписать одну запись индекса (создание и переименование).
  void IndexCharacterName(data::Uid uid, const std::string& name);
  //! Убрать запись индекса.
  void ForgetCharacterName(data::Uid uid);
  //! Перестроить индекс имён аккаунтов с диска. Зовётся только из `Initialize`.
  void RebuildUserNameIndex();
  //! Внести одну запись индекса аккаунтов (регистрация и любое сохранение).
  void IndexUserName(const std::string& name);
};

} // namespace server

#endif // FILEDATASOURCE_HPP
