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

#ifndef DATADEFINITIONS_HPP
#define DATADEFINITIONS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_set>
#include <set>
#include <map>

namespace server
{

namespace dao
{

template <typename T>
struct Field
{
  //! Constructs a field with an initialized value.
  //! @param value Value.
  Field(T value) noexcept
    : _value(std::move(value))
  {
  }

  //! Constructs field with an initialized value.
  Field()
    : _value()
  {
  }

  //! Deleted copy constructor.
  Field(const Field& field) = delete;
  //!  Deleted copy assignment operator.
  Field& operator=(const Field& field) = delete;

  Field(Field&& field) noexcept
    : _modified(field.IsModified())
    , _value(field._value)
  {
  }

  Field& operator=(Field&& field) noexcept
  {
    _modified = field.IsModified();
    _value = std::move(field._value);

    return *this;
  }


  [[nodiscard]] bool IsModified() const noexcept
  {
    return _modified;
  }

  T& operator()(const T& value) noexcept
  {
    _modified = true;
    _value = value;
    return value;
  }

  T& operator()(T&& value) noexcept
  {
    _modified = true;
    _value = std::move(value);
    return value;
  }

  const T& operator()() const noexcept
  {
    return _value;
  }

  T& operator()() noexcept
  {
    return _value;
  }

private:
  std::atomic_bool _modified{false};
  T _value;
};

} // namespace dao

namespace data
{

//! Unique identifier.
using Uid = uint32_t;
//! Type identifier.
using Tid = uint32_t;
//! Value of an invalid unique identifier.
constexpr Uid InvalidUid = 0;
//! Value of an invalid type identifier.
constexpr Tid InvalidTid = 0;

using Clock = std::chrono::system_clock;

//! User
struct User
{
  //! A name of the user.
  dao::Field<std::string> name{};
  //! An authorization token of the user.
  dao::Field<std::string> token{};
  //! Infractions.
  dao::Field<std::vector<Uid>> infractions{};
  //! A character UID of the user.
  dao::Field<Uid> characterUid{InvalidUid};
  //! The last time the user was seen online. 1 means currently online.
  dao::Field<Clock::time_point> lastSeenOnline{};
  //! LOA-fix (#18c): растянутый хеш пароля (hex) и соль (hex). Пусто =
  //! пароль не задан (легаси-аккаунт → grandfather на первом входе).
  dao::Field<std::string> passwordHash{};
  dao::Field<std::string> passwordSalt{};
};

//! Infraction
struct Infraction
{
  enum class Punishment
  {
    None, Mute, Ban
  };

  dao::Field<Uid> uid{InvalidUid};
  dao::Field<std::string> description;
  dao::Field<Punishment> punishment{Punishment::None};
  dao::Field<std::chrono::seconds> duration;
  dao::Field<Clock::time_point> createdAt;
};

//! Item
struct Item
{
  //! A unique identifier.
  dao::Field<Uid> uid{InvalidUid};
  //! A type identifier.
  dao::Field<Tid> tid{InvalidTid};
  //! An amount of an item.
  dao::Field<uint32_t> count{};
  //! A duration of an item.
  dao::Field<std::chrono::seconds> duration{};
  //! A time point of when the item was created.
  dao::Field<Clock::time_point> createdAt{};
};

//! Pet
struct Pet
{
  //! A unique identifier.
  dao::Field<Uid> uid{InvalidUid};
  //! A Item tied to the pet.
  dao::Field<Uid> itemUid{InvalidUid};
  //! A pet identifier.
  dao::Field<Uid> petId{0};
  //! A name of the pet.
  dao::Field<std::string> name{};
  //! A birth date of the pet.
  dao::Field<Clock::time_point> birthDate{};
};

//! Stored item
struct StorageItem
{
  struct Item
  {
    Tid tid{InvalidTid};
    uint32_t count{};
    std::chrono::seconds duration{};
  };

  //! A unique identifier.
  dao::Field<Uid> uid{InvalidUid};
  dao::Field<std::string> sender{};
  dao::Field<std::string> message{};
  dao::Field<int32_t> carrots{};
  dao::Field<std::vector<Item>> items{};
  dao::Field<bool> checked{false};
  dao::Field<Clock::time_point> createdAt{};
  dao::Field<std::chrono::seconds> duration{};

  dao::Field<uint32_t> goodsSq{};
  dao::Field<uint32_t> priceId{};
};

//! Guild
struct Guild
{
  dao::Field<Uid> uid{InvalidUid};
  dao::Field<std::string> name{};
  dao::Field<std::string> description{};
  dao::Field<Uid> owner{};
  dao::Field<std::vector<Uid>> officers{};
  dao::Field<std::vector<Uid>> members{};

  dao::Field<uint32_t> rank{};
  dao::Field<uint32_t> totalWins{};
  dao::Field<uint32_t> totalLosses{};
  dao::Field<uint32_t> seasonalWins{};
  dao::Field<uint32_t> seasonalLosses{};
};

//! Settings
struct Settings
{
  dao::Field<Uid> uid{InvalidUid};

  struct Option
  {
    uint32_t primaryKey{0};
    uint32_t type{0};
    uint32_t secondaryKey{0};
  };

  dao::Field<std::optional<std::vector<Option>>> keyboardBindings{std::nullopt};
  dao::Field<std::optional<std::array<std::string, 8>>> macros{std::nullopt};
  dao::Field<std::optional<std::vector<Option>>> gamepadBindings{std::nullopt};

  dao::Field<uint32_t> age{};
  dao::Field<bool> hideAge{true};
};

//! User
//! LOA (R75, #14): предел числа пер-курсовых рекордов у персонажа. Провод пишет
//! длину списка одним байтом (LobbyMessageDefinitions.cpp), поэтому 255 — не «с
//! запасом», а физический потолок кадра. Боевых карт в courses.yaml 55, так что
//! список длиннее означает порченый файл, а не игрока-рекордсмена.
constexpr std::size_t MaxCourseRecords = 255;

struct Character
{
  //! An UID of the character.
  dao::Field<Uid> uid{InvalidUid};
  //! A name of the character.
  dao::Field<std::string> name{};

  dao::Field<std::string> introduction{};

  dao::Field<uint32_t> level{};
  dao::Field<uint32_t> experience{};
  dao::Field<int32_t> carrots{};
  dao::Field<int32_t> cash{};

  enum class Role
  {
    User,
    Op,
    GameMaster
  };
  dao::Field<Role> role{};

  //! Role privilege rank.
  //! None: regular user, no staff powers.
  //! Trial: mute and temporary bans (up to 30 days).
  //! Moderator: mute and any type of ban.
  //! Admin: any admin command, including carrots and promoting/demoting.
  enum class RoleRank
  {
    None,
    Trial,
    Moderator,
    Admin
  };

  //! Regular users are always None; a rank is only granted via promotion.
  dao::Field<RoleRank> roleRank{RoleRank::None};

  struct Parts
  {
    //! An ID of the character model.
    dao::Field<uint32_t> modelId{0u};
    //! An ID of the mouth part.
    dao::Field<uint32_t> mouthId{0u};
    //! An ID of the face part.
    dao::Field<uint32_t> faceId{0u};
  } parts{};

  struct Appearance
  {
    //! An ID of the Voice model.
    dao::Field<uint32_t> voiceId{0u};
    dao::Field<uint32_t> headSize{0u};
    dao::Field<uint32_t> height{0u};
    dao::Field<uint32_t> thighVolume{0u};
    dao::Field<uint32_t> legVolume{0u};
    //! An ID of the emblem.
    dao::Field<uint32_t> emblemId{0u};
  } appearance{};

  dao::Field<Uid> guildUid{InvalidUid};

  struct Contacts
  {
    struct Group
    {
      Uid uid{};
      std::string name{};
      std::set<Uid> members{};
      Clock::time_point createdAt{};
    };

    dao::Field<std::set<Uid>> pending{};
    dao::Field<std::map<Uid, Group>> groups{};
  } contacts{};
  
  dao::Field<std::vector<Uid>> gifts{};
  dao::Field<std::vector<Uid>> purchases{};
  
  dao::Field<std::vector<Uid>> inventory{};
  dao::Field<std::vector<Uid>> characterEquipment{};
  dao::Field<std::vector<Uid>> expiredEquipment{};
  
  dao::Field<std::vector<Uid>> horses{};
  dao::Field<uint8_t> horseSlotCount{0u};

  dao::Field<std::set<Uid>> breedingWishlist{};

  dao::Field<std::vector<Uid>> pets{};
  dao::Field<Uid> mountUid{InvalidUid};
  dao::Field<Uid> petUid{InvalidUid};

  dao::Field<std::vector<Uid>> eggs{};

  dao::Field<std::vector<Uid>> housing{};

  dao::Field<bool> isRanchLocked{};

  // LOA-fix (R45-1, #58/R2): фундамент достижений. Форма взята из апстримового
  // PR #281, чтобы порт реестра и системы лёг поверх без переделки схемы.
  //! Три tid'а «главных значков» карточки персонажа; 0 = пустой слот. Клиент
  //! не требует, чтобы они были заработаны, и допускает повторы.
  dao::Field<std::array<uint16_t, 3>> keyAchievements{};

  //! Прогресс одного достижения.
  struct AchievementEntry
  {
    //! tid из клиентской таблицы `Achievements`.
    uint16_t tid{};
    //! Накопленный прогресс к порогам тиров.
    uint32_t progress{};
    //! Момент взятия каждого из четырёх тиров; эпоха = тир не взят. ★ЧИСЛО
    //! непустых = достигнутый тир, поэтому сам тир НЕ хранится: одно состояние
    //! вместо двух, и рассинхрону между ними неоткуда взяться.
    std::array<Clock::time_point, 4> tierEarnedAt{};
  };
  dao::Field<std::vector<AchievementEntry>> achievements{};

  //! Состояние наград за грейды одной книги достижений. Сам грейд книги не
  //! хранится — он следует из достижений, которые в неё входят.
  struct AchievementBookEntry
  {
    //! Номер книги, интервал <0, 8>.
    uint8_t bookId{};
    //! Ноль = награда за этот грейд не забрана. Смысл несёт только
    //! ноль/не-ноль; что именно лежит в забранной записи — открыто.
    std::array<uint32_t, 4> tierRewardClaimed{};
  };
  dao::Field<std::vector<AchievementBookEntry>> achievementBooks{};

  dao::Field<Uid> settingsUid{InvalidUid};

  struct Skills
  {
    // TODO: confirm this
    //! Max 2 skill sets per gamemode
    struct Sets
    {
      //! Max 2 skills per skill set
      struct Set
      {
        uint32_t slot1{};
        uint32_t slot2{};
      };

      Set set1{};
      Set set2{};
      uint32_t activeSetId{0};
    };

    dao::Field<Sets> speed{};
    dao::Field<Sets> magic{};
  } skills{};

    dao::Field<Uid> dailyQuestGroupUid{InvalidUid};
  struct Mailbox
  {
    dao::Field<bool> hasNewMail{false};
    dao::Field<std::vector<Uid>> inbox{};
    dao::Field<std::vector<Uid>> sent{};
  } mailbox{};

  dao::Field<std::vector<Uid>> quests{};

  //! LOA (batch2): care-skill («Уход») state. Mirrors the login-payload model
  //! ManagementSkills{class, progress, points} + SkillRanks{vector<{id,rank}>}.
  //! Migration is zero-touch: old character files lack the "careSkills" key and
  //! load as all-zero / empty (see FileDataSource).
  struct CareSkills
  {
    //! One learned care skill and its current rank (login SkillRanks::Skill).
    struct LearnedSkill
    {
      uint8_t id{};
      uint8_t rank{};
    };

    //! Spendable care points («очки ухода»).
    dao::Field<uint32_t> carePoints{};
    //! Care-class («Смотритель») level, CareSkillLevel key.
    dao::Field<uint8_t> careClassLevel{};
    //! Care-class experience toward the next level (max ~2675).
    dao::Field<uint32_t> careProgress{};
    //! Learned care skills with their current rank.
    dao::Field<std::vector<LearnedSkill>> learnedRanks{};
  } careSkills{};

  //! LOA (R65, backlog #175): ссылка, СНЯТАЯ с персонажа как нечитаемая.
  //!
  //! ★ЗАЧЕМ ПОЛЕ, А НЕ ПРОСТО СТИРАНИЕ. Уборка нечитаемых ссылок нужна: без неё
  //! персонаж перестаёт грузиться совсем. Но раньше она стирала ссылку НАСОВСЕМ
  //! и сохраняла персонажа обратно — и с этого момента возврат файла предмета из
  //! бэкапа уже ничего не давал, потому что персонаж на него больше не
  //! ссылался. Здесь ссылка уходит из живой коллекции (персонаж заходит, как и
  //! прежде), но остаётся В ЗАПИСИ, то есть операция перестала быть необратимой.
  //!
  //! ★Почему в самой записи, а не в боковом журнале: карантин обязан меняться
  //! АТОМАРНО вместе с коллекцией, из которой ссылка ушла. Отдельный файл — это
  //! второй источник правды, который разъедется с первым при первом же сбое.
  //!
  //! Миграция нулевая в обе стороны: старые файлы ключа не имеют и читаются как
  //! пустой карантин, а пустой карантин НЕ СЕРИАЛИЗУЕТСЯ вовсе (см.
  //! FileDataSource) — у здорового персонажа файл не меняется ни на байт.
  struct DamagedReference
  {
    //! UID снятой записи.
    Uid uid{InvalidUid};
    //! Вид ссылки теми же словами, что в логе уборки: «horse», «inventory
    //! item», «egg», «mount»…
    std::string kind{};
  };
  dao::Field<std::vector<DamagedReference>> damagedReferences{};

  //! LOA (R75, #14): РЕКОРД ПЕРСОНАЖА НА ОДНОЙ ТРАССЕ.
  //! ★Структура ПЛОСКАЯ (обычные поля, а не dao::Field) — как AchievementEntry:
  //! у dao::Field удалён копирующий конструктор, и вектор из них не собрался бы.
  //! ★Поля дописаны В КОНЕЦ структуры: смещения существующих членов Character
  //! не двигаются, поэтому код функций, которые раунд не правит (в том числе
  //! контроль лесенки HandleRequestMountInfo), не меняет размер.
  struct CourseRecord
  {
    //! `mapBlockId` трассы (courses.yaml -> mapBlockInfo.id; максимум 20009,
    //! в uint16 помещается). Он же уходит клиенту как
    //! AcCmdLCPersonalInfo::CourseInformation::Course::courseId.
    uint16_t courseId{};
    //! Лучшее ВАЛИДНОЕ время прохождения, мс. 0 = рекорда ещё нет.
    uint32_t recordTime{};
    //! Сколько раз трасса пройдена до финиша.
    uint32_t timesRaced{};
  };
  //! Пер-курсовые рекорды. Порядок — порядок первого финиша, клиент сортирует сам.
  dao::Field<std::vector<CourseRecord>> courseRecords{};

  //! LOA (R75, #14): заезды, доведённые до финиша, по режимам. `totalGames`
  //! ОТДЕЛЬНЫМ полем НЕ хранится — он сумма этих двух, и потому не умеет
  //! разойтись со слагаемыми. Туториальные заезды (GameMode::Tutorial) сюда не
  //! идут: окно клиента знает только «скоростные» и «магические».
  dao::Field<uint32_t> totalSpeedGames{};
  dao::Field<uint32_t> totalMagicGames{};
};

struct Horse
{
  //! A horse type.
  enum class Type
  {
    //! An adult horse.
    Adult,
    //! A horse foal.
    Foal,
    //! An adult horse which is registered in the breeding market.
    Stallion,
    //! An adult horse which is rented.
    Rent
  };

  dao::Field<Uid> uid{InvalidUid};
  dao::Field<Tid> tid{InvalidTid};
  dao::Field<std::string> name{};

  struct Parts
  {
    dao::Field<Tid> skinTid{0u};
    dao::Field<Tid> faceTid{0u};
    dao::Field<Tid> maneTid{0u};
    dao::Field<Tid> tailTid{0u};
  } parts{};

  struct Appearance
  {
    dao::Field<uint32_t> scale{0u};
    dao::Field<uint32_t> legLength{0u};
    dao::Field<uint32_t> legVolume{0u};
    dao::Field<uint32_t> bodyLength{0u};
    dao::Field<uint32_t> bodyVolume{0u};
  } appearance{};

  struct Stats
  {
    dao::Field<uint32_t> agility{0u};
    dao::Field<uint32_t> courage{0u};
    dao::Field<uint32_t> rush{0u};
    dao::Field<uint32_t> endurance{0u};
    dao::Field<uint32_t> ambition{0u};
  } stats{};

  struct Mastery
  {
    dao::Field<uint32_t> spurMagicCount{0u};
    dao::Field<uint32_t> jumpCount{0u};
    dao::Field<uint32_t> slidingTime{0u};
    dao::Field<uint32_t> glidingDistance{0u};
  } mastery{};

  dao::Field<uint32_t> rating{0u};
  //! A class.
  dao::Field<uint32_t> clazz{0u};
  //! A class progress experience points.
  dao::Field<uint32_t> clazzProgress{0u};
  dao::Field<uint32_t> grade{0u};
  dao::Field<uint32_t> growthPoints{0u};

  //! A count of how many times the horse was bred.
  dao::Field<uint32_t> breedingCount{0u};
  //! A count of successful consecutive breeds.
  dao::Field<uint32_t> breedingCombo{0u};

  dao::Field<Type> type{Type::Adult};
  dao::Field<Clock::time_point> dateOfBirth{};

  dao::Field<uint32_t> tendency{0u};
  dao::Field<uint32_t> spirit{0u};

  struct Potential
  {
    //! A type of potential.
    dao::Field<uint32_t> type{0u};
    //! A potential level represents the growth progress
    //! of the potential's value.
    dao::Field<uint32_t> level{0u};
    //! A potential value represents the intensity of the
    //! potential.
    dao::Field<uint32_t> value{0u};
  } potential{};

  dao::Field<uint32_t> luckState{0u};
  dao::Field<uint32_t> fatigue{0u};
  dao::Field<uint32_t> emblemUid{0u};

  struct MountCondition
  {
    dao::Field<uint32_t> stamina{};
    dao::Field<uint32_t> charm{};
    dao::Field<uint32_t> friendliness{};
    dao::Field<uint32_t> injury{};
    dao::Field<uint32_t> plenitude{};
    dao::Field<uint32_t> bodyDirtiness{};
    dao::Field<uint32_t> maneDirtiness{};
    dao::Field<uint32_t> tailDirtiness{};
    dao::Field<uint32_t> bodyPolish{};
    dao::Field<uint32_t> manePolish{};
    dao::Field<uint32_t> tailPolish{};
    dao::Field<uint32_t> attachment{};
    dao::Field<uint32_t> boredom{};
    dao::Field<uint32_t> stopAmendsPoint{};
  } mountCondition{};

  struct MountInfo
  {
    dao::Field<uint32_t> boostsInARow{};
    dao::Field<uint32_t> winsSpeedSingle{};
    dao::Field<uint32_t> winsSpeedTeam{};
    dao::Field<uint32_t> winsMagicSingle{};
    dao::Field<uint32_t> winsMagicTeam{};

    // Store in metres, displayed in kilometres
    dao::Field<uint32_t> totalDistance{};
    // Whole number, divided by 10 for the floating point.
    dao::Field<uint32_t> topSpeed{};
    // Whole number, divided by 10 for the floating point.
    dao::Field<uint32_t> longestGlideDistance{};

    // refers to carnival participation
    dao::Field<uint32_t> participated{};
    dao::Field<uint32_t> cumulativePrize{};
    dao::Field<uint32_t> biggestPrize{};
  } mountInfo{};

  struct Ancestors
  {
    Uid father{InvalidUid};
    Uid mother{InvalidUid};
  } ancestors{};

  //! A value in an interval of <1, 9>.
  //! Basically a weighted score of number of ancestors that share the same coat as the horse.
  //! Ancestors of the first generation add two points to the lineage,
  //! ancestors of the second generation add one point to the lineage
  //! while the horse itself adds 1.
  dao::Field<uint32_t> lineage{1u};
};

struct Housing
{
  dao::Field<Uid> uid{InvalidUid};
  dao::Field<uint32_t> housingId{};
  dao::Field<Clock::time_point> expiresAt{};
  dao::Field<uint32_t> durability{};
};

struct Egg
{
  dao::Field<Uid> uid{InvalidUid};
  dao::Field<Uid> itemUid{InvalidUid};
  dao::Field<Tid> itemTid{InvalidTid};
  dao::Field<Clock::time_point> incubatedAt{};
  dao::Field<uint32_t> incubatorSlot{};
  dao::Field<uint32_t> boostsUsed;
};

struct DailyQuestEntry
{
  //! Template ID of the quest.
  uint16_t questId{};
  //! Current progress toward the quest's successValue.
  uint32_t progress{};
};

struct DailyQuestGroup
{
  dao::Field<Uid> uid{InvalidUid};
  //! Reward entry ID shared by all 3 quests, references quests.rewards in quests.yaml.
  dao::Field<uint8_t> rewardId{};
  //! Reward type shared by all 3 quests: 1 = carrots, 2 = exp.
  dao::Field<uint8_t> rewardType{};
  //! Accumulated quest reward points. References QuestRewardPoint thresholds in quests.yaml.
  dao::Field<uint32_t> rewardPoints{};
  //! Whether the daily quest carrot reward has been claimed today.
  dao::Field<bool> carrotsClaimed{false};
  //! The 3 daily quest slots.
  dao::Field<std::array<DailyQuestEntry, 3>> quests{};
  //! LOA-fix (batch1 task3): day-index (days since the Unix epoch, day boundary
  //! at 06:00 UTC) of the last daily-quest reset. 0 = never reset. Old JSON
  //! files lack this key and load as 0, so the first ranch-enter after deploy
  //! fires exactly one reset.
  dao::Field<uint32_t> lastResetDate{0};
  //! LOA-fix (F2, quest-batch-1): whether the DAILY GROUP reward (the
  //! QuestRewardPoint threshold loot claimed via AcCmdCRRequestDailyQuestReward
  //! 0x350) has already been handed out for the current game day. There was no
  //! such flag at all, so the loot could be re-claimed indefinitely. Reset by
  //! ResetDailyQuestsIfNeeded and by a new-day registration of the group. Old
  //! JSON files lack this key and load as false.
  dao::Field<bool> dailyRewardClaimed{false};
  //! LOA-fix (R17-cap, quest-batch-2, #8): horse class-exp CLAIMED from daily quests
  //! today, per account (capped at DailyClassExpCap = one horse class/day). "Claimed",
  //! not "applied": a missing/max-class mount still consumes it (fail-closed anti-farm —
  //! never grants MORE than the cap, mount-independent). Reset on the daily rollover by
  //! BOTH entry points (ResetDailyQuestsIfNeeded on login + the fillGroup new-day path).
  //! Old JSON without this key loads as 0.
  dao::Field<uint32_t> dailyClassExpGranted{0};
  //! LOA-fix (R42, #8 F2): день последнего сброса СЧЁТЧИКА класс-опыта (индекс
  //! игрового дня, граница 06:00 UTC). ОТДЕЛЬНАЯ от lastResetDate (та — для квест-
  //! сброса): счётчик капа сбрасывается независимо от квестов, поэтому spend-путь
  //! может само-исцелить его на границе дня, НЕ трогая lastResetDate (иначе подавил
  //! бы квест-сброс). Старый JSON без ключа грузится как 0 → первый spend само-сбросит.
  dao::Field<uint32_t> dailyClassExpResetDate{0};
};

struct Quest
{
  enum class Status : uint32_t
  {
    InProgress = 0,
    ReadyToClaim = 1,
    Completed = 3
  };

  dao::Field<Uid> uid{InvalidUid};
  dao::Field<uint32_t> questId{};
  dao::Field<Status> isCompleted{Status::InProgress};
  dao::Field<uint32_t> progress{};
};

struct Mail
{
  //! Mail type.
  //! Dictates whether or not the inbox mail can be replied to, including system mails, or contains rewards.
  enum class MailType : uint32_t
  {
    CanReply = 0,
    NoReply = 1,
    CarnivalReward = 2, //! Requests AcCmdCLRequestFestivalResult
    BreedingReward = 3, //! Requests AcCmdCRBreedingTakeMoney
  };

  dao::Field<Uid> uid{InvalidUid};
  dao::Field<Uid> from{InvalidUid};
  dao::Field<Uid> to{InvalidUid};

  dao::Field<bool> isRead{false};
  dao::Field<bool> isDeleted{false};

  dao::Field<MailType> type{};
  //! The UID of either breeding or carnival reward.
  //! Non-zero values indicate system mail.
  dao::Field<uint32_t> claimUid{};

  dao::Field<Clock::time_point> createdAt{};
  dao::Field<std::string> body{};
};

struct Stallion
{
  dao::Field<Uid> uid{InvalidUid};
  dao::Field<Uid> horseUid{InvalidUid};     // The horse being registered as stallion
  dao::Field<Uid> ownerUid{InvalidUid};     // Owner of the stallion
  dao::Field<uint32_t> breedingCharge{};    // Price in carrots to breed with this stallion
  dao::Field<uint32_t> timesMated{0u};      // Times bred during current registration
  dao::Field<Clock::time_point> registeredAt{};
  dao::Field<Clock::time_point> expiresAt{};
};

struct Reward
{
  enum class Type : uint32_t
  {
    Breeding = 0,
    Carnival = 1
  };

  dao::Field<Uid> claimUid{InvalidUid};
  dao::Field<Uid> characterUid{InvalidUid};
  dao::Field<Type> type{Type::Breeding};
  dao::Field<uint32_t> carrots{0u};
  dao::Field<bool> isClaimed{false};
  dao::Field<Clock::time_point> createdAt{};
  dao::Field<Clock::time_point> claimedAt{};
};

} // namespace data

} // namespace server

#endif // DATADEFINITIONS_HPP
