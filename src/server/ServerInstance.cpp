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

#include "server/ServerInstance.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/system/QuestSystem.hpp"

#include <stacktrace>

namespace server
{

namespace
{
void DumpStackTrace()
{
  for (const auto& entry : std::stacktrace::current())
  {
    server::util::QuietLogError("[Stack] {}({}): {}", entry.source_file(), entry.source_line(), entry.description());
  }
}

//! LOA-fix (R49-3a, round49, backlog #178): доклад о падении потока, который
//! САМ НЕ МОЖЕТ БРОСИТЬ.
//! Он зовётся из веток перехвата, стоящих прямо под функцией потока, а бросок
//! из функции потока — это std::terminate ВСЕГДА, даже без noexcept. Бросить
//! же здесь есть чему: spdlog перебрасывает наружу не-std исключение приёмника
//! (SPDLOG_LOGGER_CATCH), а std::stacktrace::current() выделяет память и умеет
//! кинуть сам. Потерять строку лога можно, поток — нет.
void ReportThreadFailure(const char* subject, const char* reason) noexcept
{
  try
  {
    server::util::QuietLogError("Unhandled exception in {}: {}", subject, reason);
    DumpStackTrace();
  }
  catch (...)
  {
  }
}

} // anon namespace

ServerInstance::ServerInstance(
  const std::filesystem::path& resourceDirectory)
  : _resourceDirectory(resourceDirectory)
  , _authenticationService(*this)
  , _dataDirector(resourceDirectory / "data")
  , _lobbyDirector(*this)
  , _messengerDirector(*this)
  , _allChatDirector(*this)
  , _privateChatDirector(*this)
  , _ranchDirector(*this)
  , _raceDirector(*this)
  , _chatSystem(*this)
  , _infractionSystem(*this)
  , _itemSystem(*this)
  , _horseSystem(*this)
  , _matchmakingSystem(*this)
  , _questSystem(*this)
  , _achievementSystem(*this)
  , _rewardSystem(*this)
  , _telemetry(*this)
  , _breedingMarket(*this)
  , _genetics(*this)
{
}

ServerInstance::~ServerInstance()
{
  // LOA-fix (R49-13, round49, backlog #178): деструктор НЕЯВНО noexcept, а внутри
  // две бросающие вещи — запись в лог (spdlog перебрасывает неизвестное
  // исключение приёмника) и сам `join()` (бросает std::system_error). То есть
  // штатное завершение сервера могло закончиться не завершением, а падением.
  // ★Имя потока принимается как `const char*`, а НЕ как `const std::string&`:
  // временная строка создавалась бы в точке вызова, то есть В ДЕСТРУКТОРЕ и вне
  // всяких перехватов, и её выделение памяти могло бы бросить само.
  const auto waitForThread = [](const char* threadName, std::thread& thread) noexcept
  {
    if (not thread.joinable())
      return;

    // Жалоба и ожидание — в РАЗНЫХ перехватах: сбой записи в лог не должен
    // отменять join, иначе объект потока останется присоединяемым, а его
    // собственный деструктор на таком объекте зовёт std::terminate.
    try
    {
      server::util::QuietLogDebug("Waiting for the '{}' thread to finish...", threadName);
    }
    catch (...)
    {
    }

    try
    {
      thread.join();
    }
    catch (...)
    {
      // ★НЕ отцепляем. Поток захватывает `this` и работает по состоянию,
      // которое прямо сейчас разрушается: отцепить такой поток — променять
      // предсказуемое падение на порчу памяти у живых игроков. Если ожидание
      // не удалось, честнее громко сказать об этом и оставить штатное
      // поведение языка.
      try
      {
        server::util::QuietLogCritical(
          "Failed to wait for the '{}' thread; shutdown is not clean", threadName);
      }
      catch (...)
      {
      }
    }

    try
    {
      server::util::QuietLogDebug("Thread for '{}' finished", threadName);
    }
    catch (...)
    {
    }
  };

  // LOA-fix (R49-16, round49, backlog #178): поток телеметрии не дожидались
  // ВООБЩЕ — при включённых метриках его живой объект убивал процесс на выходе
  // через деструктор самого std::thread. Стартует он последним, поэтому и ждём
  // его первым.
  waitForThread("telemetry", _telemetryThread);
  waitForThread("race director", _raceDirectorThread);
  waitForThread("ranch director", _ranchDirectorThread);
  waitForThread("private chat director", _privateChatDirectorThread);
  waitForThread("all chat director", _allChatDirectorThread);
  waitForThread("messenger director", _messengerThread);
  waitForThread("lobby director", _lobbyDirectorThread);
  waitForThread("data director", _dataDirectorThread);
  waitForThread("authentication", _authenticationThread);
}

void ServerInstance::Initialize()
{
  _shouldRun.store(true, std::memory_order::release);

  // Load configurations from file system.
  LoadConfigurations();
  // Load configurations from environment variables.
  _config.LoadFromEnvironment();

  // Initialize the directors and tick them on their own threads.
  // Directors will terminate their tick loop once `_shouldRun` flag is set to false.

  // Authentication service
  _authenticationThread = std::thread([this]()
  {
    try
    {
      _authenticationService.Initialize();
      RunDirectorTaskLoop(_authenticationService);
      _authenticationService.Terminate();
    }
    catch (const std::exception& x)
    {
      ReportThreadFailure("the authentication", x.what());
      _shouldRun = false;
    }
    catch (...)
    {
      // Не-std исключение раньше уходило мимо перехвата прямо из функции
      // потока — то есть валило весь сервер. Теперь оно гасит один поток
      // тем же способом, что и обычное: флаг работы снят, остальные
      // потоки завершаются штатно.
      ReportThreadFailure("the authentication", "unknown exception");
      _shouldRun = false;
    }
  });

  // Data director
  _dataDirectorThread = std::thread([this]()
  {
    try
    {
      _dataDirector.Initialize();
      RunDirectorTaskLoop(_dataDirector);
      _dataDirector.Terminate();
    }
    catch (const std::exception& x)
    {
      ReportThreadFailure("the data director", x.what());
      _shouldRun = false;
    }
    catch (...)
    {
      // Не-std исключение раньше уходило мимо перехвата прямо из функции
      // потока — то есть валило весь сервер. Теперь оно гасит один поток
      // тем же способом, что и обычное: флаг работы снят, остальные
      // потоки завершаются штатно.
      ReportThreadFailure("the data director", "unknown exception");
      _shouldRun = false;
    }
  });

  // Lobby director
  _lobbyDirectorThread = std::thread([this]()
  {
    try
    {
      _lobbyDirector.Initialize();
      RunDirectorTaskLoop(_lobbyDirector);
      _lobbyDirector.Terminate();
    }
    catch (const std::exception& x)
    {
      ReportThreadFailure("the lobby director", x.what());
      _shouldRun = false;
    }
    catch (...)
    {
      // Не-std исключение раньше уходило мимо перехвата прямо из функции
      // потока — то есть валило весь сервер. Теперь оно гасит один поток
      // тем же способом, что и обычное: флаг работы снят, остальные
      // потоки завершаются штатно.
      ReportThreadFailure("the lobby director", "unknown exception");
      _shouldRun = false;
    }
  });

  // Messenger director
  if (_config.messenger.enabled)
  {
    _messengerThread = std::thread([this]()
    {
      try
      {
        _messengerDirector.Initialize();
        RunDirectorTaskLoop(_messengerDirector);
        _messengerDirector.Terminate();
      }
      catch (const std::exception& x)
      {
        ReportThreadFailure("the messenger director", x.what());
        _shouldRun = false;
      }
      catch (...)
      {
        // Не-std исключение раньше уходило мимо перехвата прямо из функции
        // потока — то есть валило весь сервер. Теперь оно гасит один поток
        // тем же способом, что и обычное: флаг работы снят, остальные
        // потоки завершаются штатно.
        ReportThreadFailure("the messenger director", "unknown exception");
        _shouldRun = false;
      }
    });

    // All chat director
    if (_config.allChat.enabled) // All chat depends on messenger
    {
      _allChatDirectorThread = std::thread([this]()
      {
        try
        {
          _allChatDirector.Initialize();
          RunDirectorTaskLoop(_allChatDirector);
          _allChatDirector.Terminate();
        }
        catch (const std::exception& x)
        {
          ReportThreadFailure("the messenger (all chat) director", x.what());
          _shouldRun = false;
        }
        catch (...)
        {
          // Не-std исключение раньше уходило мимо перехвата прямо из функции
          // потока — то есть валило весь сервер. Теперь оно гасит один поток
          // тем же способом, что и обычное: флаг работы снят, остальные
          // потоки завершаются штатно.
          ReportThreadFailure("the messenger (all chat) director", "unknown exception");
          _shouldRun = false;
        }
      });
    }

    // Private chat director
    if (_config.privateChat.enabled) // Private chat depends on messenger
    {
      _privateChatDirectorThread = std::thread([this]()
      {
        try
        {
          _privateChatDirector.Initialize();
          RunDirectorTaskLoop(_privateChatDirector);
          _privateChatDirector.Terminate();
        }
        catch (const std::exception& x)
        {
          ReportThreadFailure("the messenger (private chat) director", x.what());
          _shouldRun = false;
        }
        catch (...)
        {
          // Не-std исключение раньше уходило мимо перехвата прямо из функции
          // потока — то есть валило весь сервер. Теперь оно гасит один поток
          // тем же способом, что и обычное: флаг работы снят, остальные
          // потоки завершаются штатно.
          ReportThreadFailure("the messenger (private chat) director", "unknown exception");
          _shouldRun = false;
        }
      });
    }
  }

  // Ranch director
  _ranchDirectorThread = std::thread([this]()
  {
    try
    {
      _ranchDirector.Initialize();
      RunDirectorTaskLoop(_ranchDirector);
      _ranchDirector.Terminate();
    }
    catch (const std::exception& x)
    {
      ReportThreadFailure("the ranch director", x.what());
      _shouldRun = false;
    }
    catch (...)
    {
      // Не-std исключение раньше уходило мимо перехвата прямо из функции
      // потока — то есть валило весь сервер. Теперь оно гасит один поток
      // тем же способом, что и обычное: флаг работы снят, остальные
      // потоки завершаются штатно.
      ReportThreadFailure("the ranch director", "unknown exception");
      _shouldRun = false;
    }
  });

  // Race director
  _raceDirectorThread = std::thread([this]()
  {
    try
    {
      _raceDirector.Initialize();
      RunDirectorTaskLoop(_raceDirector);
      _raceDirector.Terminate();
    }
    catch (const std::exception& x)
    {
      ReportThreadFailure("the race director", x.what());
      _shouldRun = false;
    }
    catch (...)
    {
      // Не-std исключение раньше уходило мимо перехвата прямо из функции
      // потока — то есть валило весь сервер. Теперь оно гасит один поток
      // тем же способом, что и обычное: флаг работы снят, остальные
      // потоки завершаются штатно.
      ReportThreadFailure("the race director", "unknown exception");
      _shouldRun = false;
    }
  });

  // Telemetry.
  if (GetSettings().telemetry.enabled)
  {
    _telemetryThread = std::thread([this]()
    {
      try
      {
        _telemetry.Initialize();
        RunDirectorTaskLoop(_telemetry);
        _telemetry.Terminate();
      }
      catch (const std::exception& x)
      {
        ReportThreadFailure("telemetry", x.what());
        _shouldRun = false;
      }
      catch (...)
      {
        // Не-std исключение раньше уходило мимо перехвата прямо из функции
        // потока — то есть валило весь сервер. Теперь оно гасит один поток
        // тем же способом, что и обычное: флаг работы снят, остальные
        // потоки завершаются штатно.
        ReportThreadFailure("telemetry", "unknown exception");
        _shouldRun = false;
      }
    });
  }
  else
  {
    server::util::QuietLogInfo("Metric collection is disabled");
  }
}

void ServerInstance::Terminate()
{
  _shouldRun.store(false, std::memory_order::relaxed);
  _breedingMarket.Terminate();
}

void ServerInstance::LoadConfigurations()
{
  // Read server configurations
  _config.LoadFromFile(_resourceDirectory / "config/server/config.yaml");
  _moderationSystem.ReadConfig(_resourceDirectory / "config/server/automod.yaml");
  _systemContentRegistry.ReadConfig(_resourceDirectory / "config/server/system_content.yaml");

  // Read game configurations
  _breedingRegistry.ReadConfig(_resourceDirectory / "config/game/breeding.yaml");
  _characterRegistry.ReadConfig(_resourceDirectory / "config/game/character.yaml");
  _courseRegistry.ReadConfig(_resourceDirectory / "config/game/courses.yaml");
  _horseRegistry.ReadConfig(_resourceDirectory / "config/game/horses");
  _itemRegistry.ReadConfig(_resourceDirectory / "config/game/items");
  _magicRegistry.ReadConfig(_resourceDirectory / "config/game/magic.yaml");
  _petRegistry.ReadConfig(_resourceDirectory / "config/game/pets.yaml");
  _questRegistry.ReadConfig(_resourceDirectory / "config/game/quests.yaml");
  _careSkillRegistry.ReadConfig(_resourceDirectory / "config/game/care_skills.yaml");
  _achievementRegistry.ReadConfig(_resourceDirectory / "config/game/achievements.yaml");

  // LOA-fix (R68, backlog #5/#99): КОНТРАКТ КОНФИГА «СОБЕРИ N ПРЕДМЕТОВ».
  //
  // ★ЗАЧЕМ ГРОМКИЙ ОТКАЗ, А НЕ ТИХИЙ ПРОПУСК. С этого раунда выплата за 12
  // сюжетных квестов класса `CollectDropItem` ГЕЙТИТСЯ прогрессом
  // (`RanchDirector::HandleRequestQuestReward`), а прогресс двигается ТОЛЬКО
  // подбором предмета, разложенного по секции `questItemDeckInfo` в
  // courses.yaml. Бинарь и данные умеют разъехаться НЕ ГИПОТЕТИЧЕСКИ: на проде
  // каталог `config` монтируется в контейнер С ХОСТА, то есть новый образ
  // спокойно поднимется со СТАРЫМ courses.yaml. В этом состоянии сервер был бы
  // «здоров» по всем признакам и МОЛЧА сделал бы 12 квестов несдаваемыми
  // навсегда — регрессия хуже того эксплойта, который раунд закрывает.
  // Поэтому расхождение обязано ронять СТАРТ, а не игру: проверка, чей вердикт
  // никто не останавливает, — не проверка ([[a-check-nobody-reads-is-not-a-check]]).
  //
  // ★ЧТО ИМЕННО ПРОВЕРЯЕТСЯ: для КАЖДОГО tid из списка — что квест есть в
  // реестре, что он действительно класса `CollectDropItem` (список не лжёт про
  // себя) и что у его `functionValue` есть ХОТЯ БЫ ОДНА точка спавна, у
  // которой на карте реально лежат координаты. «Секция есть» не проверяется —
  // проверяется ДОСТИЖИМОСТЬ, потому что именно она и требуется.
  {
    std::vector<uint32_t> unreachableQuests;
    for (const uint32_t questTid : QuestSystem::CollectDropItemMainQuestTids)
    {
      const auto questTemplate = _questRegistry.GetQuest(questTid);
      if (not questTemplate.has_value()
        || questTemplate->function != registry::Quest::Function::CollectDropItem)
      {
        unreachableQuests.push_back(questTid);
        continue;
      }

      bool isReachable = false;
      for (const auto& questItemDeck : _courseRegistry.GetQuestItemDecks())
      {
        if (questItemDeck.questItemId != questTemplate->functionValue
          || questItemDeck.spawnCount == 0)
          continue;

        for (const auto& spawnPoint : questItemDeck.spawnPoints)
        {
          try
          {
            const auto& mapBlockInfo = _courseRegistry.GetMapBlockInfo(
              spawnPoint.mapBlockId);
            for (const auto& deckInstance : mapBlockInfo.itemDecks)
            {
              if (deckInstance.deckId == spawnPoint.deckId)
              {
                isReachable = true;
                break;
              }
            }
          }
          catch (const std::exception&)
          {
            // Карты нет в реестре — эта точка спавна просто не считается
            // достижимой. Дека 701 ивентовых квестов живёт именно так.
          }

          if (isReachable)
            break;
        }

        if (isReachable)
          break;
      }

      if (not isReachable)
        unreachableQuests.push_back(questTid);
    }

    if (not unreachableQuests.empty())
    {
      std::string questList;
      for (const uint32_t questTid : unreachableQuests)
      {
        if (not questList.empty())
          questList += ", ";
        questList += std::to_string(questTid);
      }

      throw std::runtime_error(
        "config/game/courses.yaml: у сюжетных квестов класса CollectDropItem нет "
        "достижимых точек спавна (" + questList + "). Секция questItemDeckInfo "
        "отсутствует или неполна, а выплата за эти квесты гейтится прогрессом — "
        "сервер не стартует, чтобы не сделать их несдаваемыми молча.");
    }
  }
}

AuthenticationService& ServerInstance::GetAuthenticationService()
{
  return _authenticationService;
}

DataDirector& ServerInstance::GetDataDirector()
{
  return _dataDirector;
}

LobbyDirector& ServerInstance::GetLobbyDirector()
{
  return _lobbyDirector;
}

RanchDirector& ServerInstance::GetRanchDirector()
{
  return _ranchDirector;
}

RaceDirector& ServerInstance::GetRaceDirector()
{
  return _raceDirector;
}

MessengerDirector& ServerInstance::GetMessengerDirector()
{
  return _messengerDirector;
}

AllChatDirector& ServerInstance::GetAllChatDirector()
{
  return _allChatDirector;
}

PrivateChatDirector& ServerInstance::GetPrivateChatDirector()
{
  return _privateChatDirector;
}

registry::CharacterRegistry& ServerInstance::GetCharacterRegistry()
{
  return _characterRegistry;
}

registry::CourseRegistry& ServerInstance::GetCourseRegistry()
{
  return _courseRegistry;
}

registry::HorseRegistry& ServerInstance::GetHorseRegistry()
{
  return _horseRegistry;
}

registry::ItemRegistry& ServerInstance::GetItemRegistry()
{
  return _itemRegistry;
}

registry::PetRegistry& ServerInstance::GetPetRegistry()
{
  return _petRegistry;
}

registry::QuestRegistry& ServerInstance::GetQuestRegistry()
{
  return _questRegistry;
}

registry::CareSkillRegistry& ServerInstance::GetCareSkillRegistry()
{
  return _careSkillRegistry;
}

registry::AchievementRegistry& ServerInstance::GetAchievementRegistry()
{
  return _achievementRegistry;
}

AchievementSystem& ServerInstance::GetAchievementSystem()
{
  return _achievementSystem;
}

registry::MagicRegistry& ServerInstance::GetMagicRegistry()
{
  return _magicRegistry;
}

registry::SystemContentRegistry& ServerInstance::GetSystemContentRegistry()
{
  return _systemContentRegistry;
}

registry::BreedingRegistry& ServerInstance::GetBreedingRegistry()
{
  return _breedingRegistry;
}

ChatSystem& ServerInstance::GetChatSystem()
{
  return _chatSystem;
}

InfractionSystem& ServerInstance::GetInfractionSystem()
{
  return _infractionSystem;
}

ItemSystem& ServerInstance::GetItemSystem()
{
  return _itemSystem;
}

HorseSystem& ServerInstance::GetHorseSystem()
{
  return _horseSystem;
}

ModerationSystem& ServerInstance::GetModerationSystem()
{
  return _moderationSystem;
}

RoomSystem& ServerInstance::GetRoomSystem()
{
  return _roomSystem;
}

MatchmakingSystem& ServerInstance::GetMatchmakingSystem()
{
  return _matchmakingSystem;
}

RewardSystem& ServerInstance::GetRewardSystem()
{
  return _rewardSystem;
}

QuestSystem& ServerInstance::GetQuestSystem()
{
  return _questSystem;
}

Telemetry& ServerInstance::GetTelemetry()
{
  return _telemetry;
}

OtpSystem& ServerInstance::GetOtpSystem()
{
  return _otpSystem;
}

Genetics& ServerInstance::GetGenetics()
{
  return _genetics;
}

BreedingMarket& ServerInstance::GetBreedingMarket()
{
  return _breedingMarket;
}

Config& ServerInstance::GetSettings()
{
  return _config;
}

} // namespace server
