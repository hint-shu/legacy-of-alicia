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

#include "libserver/data/file/FileDataSource.hpp"
#include "libserver/util/QuietLog.hpp"
#include <algorithm>
#include <limits>
#include <ranges>
#include "libserver/util/AtomicFile.hpp"
// LOA-fix (R73-3, #130-C8): структурный гейт имени и ASCII-сравнение вместо
// регулярного выражения, собиравшегося из присланного клиентом имени. ★Само
// слово-токен здесь не пишется: гейт `tools/no_name_regex_gate.sh` ищет его по
// всему `src/libserver/data/`, и упоминание в комментарии сделало бы гейт
// красным на честном коде.
#include "libserver/util/NameGuard.hpp"

#include <cctype>
#include <chrono>
#include <format>
#include <optional>

#include <spdlog/spdlog.h>

#include <nlohmann/json.hpp>

namespace
{

//! ПРЕДЕЛ ЧАСТОТЫ СВЕРКИ ИНДЕКСА АККАУНТОВ С ДИСКОМ (правка ревью, итерация 3).
//!
//! ★ЭТО НЕ «ТЮНИНГ», А ГРАНИЦА КЛАССА. Промах индекса имеет право переспросить
//! диск, иначе аккаунт, заведённый рядом с работающим сервером, не находился бы
//! до перезапуска. Но право «переспросить» без потолка означает, что стоимость
//! пакета снова становится O(число аккаунтов): staff-клиент, чередуя
//! сохранение записи (оно меняет mtime каталога) с запросом отсутствующего
//! имени, заказывает по полному обходу на пару. Потолок превращает «на пакет» в
//! «не чаще раза в пять секунд», сколько бы пакетов и потоков ни пришло.
constexpr auto kUserIndexReconcileGap = std::chrono::seconds(5);

//! ПОЛ ЧАСТОТЫ: раз в минуту промах сверяется с диском ДАЖЕ при совпавшем
//! отпечатке.
//!
//! ★ОТПЕЧАТОК КАТАЛОГА НЕ СВОБОДЕН ОТ КОЛЛИЗИЙ (найдено ревью, итерация 3).
//! Файловая система с грубым разрешением времени, восстановление из копии,
//! возвращающее каталогу прежний mtime, — и новый файл живёт под старым
//! отпечатком. Признак «изменилось» тогда молчит НАВСЕГДА. Принудительная
//! сверка ограничивает это молчание минутой и стоит один обход в минуту — и
//! только при промахах, то есть только когда кто-то спрашивает.
constexpr auto kUserIndexStaleAfter = std::chrono::seconds(60);

//! ПРЕДЕЛ ЧАСТОТЫ ПОПЫТОК ПЕРЕСОБРАТЬ СЛОМАННЫЙ ИНДЕКС ИМЁН (правка ревью,
//! итерация 7).
//!
//! ★ЗАЧЕМ ВООБЩЕ ПОПЫТКИ. Индекс, объявленный неполным, отвечает «занято» на
//! ЛЮБОЕ имя — это безопасно, но это и остановка создания персонажей и гильдий.
//! Без пути к самопочинке один файл, который был нечитаем ровно в момент
//! старта, стоил бы отказа в создании ДО ПЕРЕЗАПУСКА, хотя на диске всё давно
//! в порядке.
//!
//! ★ЗАЧЕМ ПОТОЛОК. Пока данные действительно битые, перестройка — это полный
//! обход каталога, и без потолка её заказывал бы КАЖДЫЙ пакет создания. Две
//! секунды дают самопочинку за время, неразличимое для человека, и при этом
//! ограничивают стоимость одним обходом в две секунды на весь сервер.
constexpr auto kBrokenNameIndexRetryGap = std::chrono::seconds(2);

//! Занимает право на попытку перестройки: `true` не чаще раза в `gap`.
//!
//! ★ЧАСЫ ЧИТАЮТСЯ БЕЗ ЗАМКА ИНДЕКСА НАМЕРЕННО. Перестройка держит замок
//! индекса эксклюзивно; спрашивать «а можно?» под тем же замком значило бы
//! встать в очередь за той самой перестройкой, которую мы пытаемся не
//! дублировать.
bool ClaimNameIndexRetry(
  std::atomic<std::chrono::steady_clock::time_point>& last,
  const std::chrono::steady_clock::duration gap)
{
  const auto now = std::chrono::steady_clock::now();
  auto previous = last.load(std::memory_order::relaxed);
  while (true)
  {
    if (previous != std::chrono::steady_clock::time_point{}
      && now - previous < gap)
    {
      return false;
    }
    if (last.compare_exchange_weak(
        previous, now, std::memory_order::relaxed, std::memory_order::relaxed))
    {
      return true;
    }
  }
}

//! РАЗБИРАЕТ uid ЗАПИСИ ИЗ ИМЕНИ ФАЙЛА (правка ревью, итерация 7).
//!
//! ★ЛИЧНОСТЬ ЗАПИСИ — ЭТО ИМЯ ФАЙЛА, А НЕ ПОЛЕ ВНУТРИ НЕГО. `StoreGuild`,
//! `DeleteGuild`, `StoreCharacter` и `DeleteCharacter` адресуют файл ИМЕНЕМ
//! (`ProduceDataFilePath(dir, std::format("{}", uid))`), а перестройка индекса
//! брала uid из `json["uid"]`. Эти два числа независимы: `7.json` с полем
//! `uid: 8` заводил обратную запись под 8, и `DeleteGuild(7)` не снимал имя —
//! оно оставалось занятым до перезапуска. Два файла с ОДНИМ полем `uid` и
//! разными именами давали обратное: живое имя становилось свободным.
//!
//! ★РАЗБОР СТРОГИЙ, ПО ТОЙ ЖЕ ПРИЧИНЕ, ЧТО В `HighestUidInDirectory`:
//! `std::stoul` принимает знак и игнорирует хвост.
//!
//! @return uid или `std::nullopt`, если имя файла не является записью.
std::optional<server::data::Uid> ParseRecordUid(const std::string& stem)
{
  if (stem.empty() || stem.size() > 10
    || not std::ranges::all_of(stem, [](const unsigned char symbol)
      {
        return symbol >= '0' && symbol <= '9';
      }))
  {
    return std::nullopt;
  }

  uint64_t parsed = 0;
  for (const char symbol : stem)
    parsed = parsed * 10 + static_cast<uint64_t>(symbol - '0');

  if (parsed == 0 || parsed >= std::numeric_limits<uint32_t>::max())
    return std::nullopt;

  return static_cast<server::data::Uid>(parsed);
}

//! УДАЛЯЕТ ЗАПИСЬ ИЛИ ГОВОРИТ ВСЛУХ, ЧТО НЕ УДАЛИЛ (правка ревью, итерация 7).
//!
//! ★ЗАЧЕМ ОТДЕЛЬНАЯ ФУНКЦИЯ, А НЕ `if` В ПЯТНАДЦАТИ МЕСТАХ. Пятнадцать копий
//! одного `if` — это список мест: шестнадцатый `Delete*`, написанный через
//! полгода по образцу соседей, унаследует ту форму, которую увидит. Здесь форма
//! ровно одна, и `[[nodiscard]]` у `RemoveManagedFile` не даёт написать другую.
//!
//! ★БРОСОК, А НЕ КОД ВОЗВРАТА, ПОТОМУ ЧТО ЕГО КТО-ТО ЛОВИТ. `DataDirector`
//! оборачивает каждый `Delete*` в `try` и на исключении возвращает `false`, а
//! `DataStorage::ProcessDeleteQueue` при `false` ОСТАВЛЯЕТ запись доступной в
//! кэше. То есть бросок здесь — это ровно та проводка, которая была у прежнего
//! `std::filesystem::remove(path)` (бросающая перегрузка), и восстановление
//! этого пути ничего нового не заводит.
void RemoveDataFileOrThrow(
  const std::filesystem::path& path, const std::string_view what)
{
  if (server::util::RemoveManagedFile(path))
    return;

  throw std::runtime_error(
    std::format(
      "{} '{}' could not be deleted",
      what, server::util::LogPath(path)));
}

//! Вносит uid в отсортированный список имени (LOA-fix R73-4, правка ревью 1).
//!
//! ★СПИСОК, А НЕ ЕДИНСТВЕННЫЙ ПОБЕДИТЕЛЬ. Столкновение имён на диске возможно
//! (ext4 регистрозависим, а ключ индекса — ASCII-нижний регистр), и прежняя
//! редакция проигравшие uid ТЕРЯЛА. Стоило победителю переименоваться или
//! удалиться — и имя переставало разрешаться до перезапуска, хотя второй файл с
//! этим именем лежал на месте. Порядок по возрастанию делает правило «меньший
//! uid — старшая запись» одинаковым при старте и в рантайме и превращает снятие
//! победителя в ДЕТЕРМИНИРОВАННОЕ повышение следующего.
//!
//! @return размер списка ПОСЛЕ вставки: 1 — имя уникально, больше — столкновение.
std::size_t AttachNameKey(
  std::unordered_map<std::string, std::vector<server::data::Uid>>& index,
  const std::string& key,
  const server::data::Uid uid)
{
  auto& bucket = index[key];
  const auto position = std::ranges::lower_bound(bucket, uid);
  // Повторная вставка того же uid — не столкновение, а тот же самый персонаж.
  if (position == bucket.end() || *position != uid)
    bucket.insert(position, uid);
  return bucket.size();
}

//! Снимает uid со списка имени; пустой список убирает ключ целиком.
void DetachNameKey(
  std::unordered_map<std::string, std::vector<server::data::Uid>>& index,
  const std::string& key,
  const server::data::Uid uid)
{
  const auto entry = index.find(key);
  if (entry == index.end())
    return;
  auto& bucket = entry->second;
  const auto position = std::ranges::lower_bound(bucket, uid);
  if (position != bucket.end() && *position == uid)
    bucket.erase(position);
  if (bucket.empty())
    index.erase(entry);
}

//! Поднимает потолок длины имени, никогда его не опуская.
void RaiseNameCeiling(std::atomic_size_t& ceiling, const std::size_t candidate)
{
  std::size_t current = ceiling.load(std::memory_order::relaxed);
  while (candidate > current
    && not ceiling.compare_exchange_weak(
      current, candidate, std::memory_order::relaxed))
  {
  }
}

//! ЕДИНСТВЕННОЕ ЧТЕНИЕ ФАЙЛА ДАННЫХ В ЭТОМ КЛАССЕ (LOA-fix R73-6, ревью 4).
//!
//! ★ЗАЧЕМ ОДНА ФУНКЦИЯ ВМЕСТО ДЕВЯТНАДЦАТИ ОДИНАКОВЫХ ТРОЙЧАТОК. Каждое чтение
//! было написано как `std::ifstream file(path)` + проверка `is_open` + разбор.
//! `ifstream` ХОДИТ ПО СИМВОЛИЧЕСКОЙ ССЫЛКЕ и открывает именованные каналы,
//! поэтому свойство «под `data/` читается только управляемая запись» было
//! ложным в девятнадцати местах сразу, а двадцатое чтение, написанное через
//! полгода, унаследовало бы дефект по образцу соседей. Теперь оно одно, и
//! свойство держится построением, а не перечнем починенных мест.
//!
//! ★ТЕКСТ БРОСКА СОХРАНЁН ДОСЛОВНО. `"{} '{}' not accessible"` — маркер лесенки
//! прошлых раундов; переписать его значило бы объявить контроль сдвинувшимся
//! там, где поведение не менялось.
nlohmann::json ReadManagedJson(
  const std::filesystem::path& path,
  const std::string_view what,
  const server::util::FileSensitivity sensitivity)
{
  const auto read = server::util::ReadManagedFile(path, sensitivity);
  if (read.status != server::util::ManagedReadStatus::Ok)
  {
    throw std::runtime_error(
      std::format("{} '{}' not accessible", what, server::util::LogPath(path)));
  }
  return nlohmann::json::parse(read.content);
}

//! ★СОЗДАНИЕ КАТАЛОГА ТОЖЕ НЕ ХОДИТ ПО ССЫЛКАМ (правка ревью, итерация 6).
//!
//! Прежняя редакция звала `exists` + `create_directories` ПО ПУТИ, то есть
//! инвариант «под `data/` не ходят по ссылкам» держался у чтения, записи и
//! обхода, но не у создания: подмена `data/characters/equipment` ссылкой на
//! чужое дерево заставляла сервер СОЗДАТЬ там `items` — и всё дальнейшее шло
//! мимо `data/`. Здесь единственное место, через которое рождаются ВСЕ пути
//! данных этого класса, поэтому и правка одна.
std::filesystem::path ProduceDataFilePath(
  const std::filesystem::path& root,
  const std::string& filename)
{
  if (not server::util::CreateManagedDirectories(root))
  {
    throw std::runtime_error(
      std::format(
        "Data directory '{}' could not be created without following a symbolic "
        "link", server::util::LogPath(root)));
  }
  return root / (filename + ".json");
}

//! Наибольший UID, ФАКТИЧЕСКИ встречающийся в именах файлов каталога
//! (LOA-fix, R58-4, round58, backlog #175).
//!
//! ★Существует, чтобы счётчик УЖЕ НИКОГДА не выдал занятый идентификатор, что бы
//! ни случилось с мета-файлом. Потерянный, обнулённый или откаченный мета
//! перестаёт быть катастрофой: пол берётся из самих данных. Приём не новый — так
//! уже устроен `ListRegisteredStallions` в этом же файле.
//! Следующий идентификатор с ПРОВЕРКОЙ ИСЧЕРПАНИЯ (LOA-fix, R58-16, #175).
//!
//! ★Апстримный `++счётчик` не проверял ничего, и это было терпимо, пока верхнее
//! значение задавал только мета-файл. Пол по фактическим именам файлов, который
//! вводит этот раунд, сделал верхнюю границу достижимой С ДИСКА: файл с именем
//! `4294967294.json` поднял бы счётчик к самому краю, и через два выпуска
//! идентификатор стал бы нулём, то есть `InvalidUid`. Иначе говоря, защита от
//! обнуления счётчиков сама открывала дорогу к обнулению. Найдено ревью
//! (итерация 2).
//!
//! Отказ ГРОМКИЙ и с откатом: выдать занятый или недействительный идентификатор
//! хуже, чем не выдать никакого.
uint32_t NextUid(std::atomic<uint32_t>& counter, const std::string_view what)
{
  // ★ПРОВЕРКА ДО ИЗМЕНЕНИЯ, А НЕ ПОСЛЕ. Прежняя редакция делала `fetch_add` и
  // откатывала счётчик, если значение оказалось негодным, — но между этими
  // двумя шагами испорченное значение УЖЕ ОПУБЛИКОВАНО, и соседний поток успел
  // бы его прочитать. А выдача идёт не с одного потока: `Create*` зовут лобби,
  // ранчо, мессенджер и сетевые потоки. Найдено ревью (итерация 3).
  //
  // Цикл сравнения-с-обменом меняет счётчик только тогда, когда следующее
  // значение заведомо годное, поэтому негодного не существует ни мгновения.
  uint32_t current = counter.load(std::memory_order::relaxed);

  for (;;)
  {
    if (current >= std::numeric_limits<uint32_t>::max() - 1)
    {
      throw std::runtime_error(
        std::format("Ran out of {} identifiers", what));
    }

    const uint32_t next = current + 1;
    if (counter.compare_exchange_weak(
          current, next, std::memory_order::relaxed, std::memory_order::relaxed))
    {
      return next;
    }
  }
}

//! Пол счётчика uid, снятый с каталога, ВМЕСТЕ с честностью снимка.
//!
//! ★ЧИСЛО БЕЗ ПРИЗНАКА ПОЛНОТЫ — ЛОЖНО-ЗЕЛЁНОЕ ПО ПОСТРОЕНИЮ (правка ревью,
//! итерация 3). Оборванный обход возвращает МЕНЬШИЙ максимум, неотличимый от
//! честного: пол 99 вместо 100 выглядит как каталог, в котором просто нет
//! сотого файла. Признак обязан ехать вместе со значением, иначе вызывающий
//! физически не может его учесть.
struct UidFloorScan
{
  uint32_t highest = 0;
  bool incomplete = false;
};

UidFloorScan HighestUidInDirectory(const std::filesystem::path& root)
{
  UidFloorScan scan;

  // ★ОБХОД ЧЕРЕЗ `ListRegularFiles` (правка ревью, итерация 2): продвижение
  // итератора в range-for бросает, а счётчик uid считается на СТАРТЕ — исключение
  // отсюда роняло бы процесс до первого игрока.
  const auto listing = server::util::ListRegularFiles(root);
  scan.incomplete = listing.incomplete;

  // ★ОТВЕРГНУТАЯ ССЫЛКА ВСЁ РАВНО ЗАНИМАЕТ ИМЯ (правка ревью, итерация 4). Пол
  // счётчика — это утверждение об ИМЕНАХ в каталоге, а не о инодах: если
  // `characters/100.json` стал ссылкой, читать её мы отказываемся, но выдать
  // uid 100 следующему персонажу нельзя тем более — запись легла бы на занятое
  // имя. Прежняя редакция ссылку молча пропускала, то есть пол опускался ровно
  // на том имени, из-за которого он и заведён.
  std::vector<std::filesystem::path> occupied = listing.files;
  occupied.insert(
    occupied.end(),
    listing.refusedSymlinks.begin(),
    listing.refusedSymlinks.end());

  for (const auto& entryPath : occupied)
  {
    if (entryPath.extension() != ".json")
      continue;

    // ★РАЗБОР СТРОГИЙ, И ЭТО НЕ ПЕДАНТИЗМ. `std::stoul` принимает знак и
    // молча игнорирует хвост, поэтому файл с именем `-1.json` дал бы
    // UINT32_MAX — а следующий `++счётчик` завернул бы его в 0, то есть в
    // `InvalidUid`. Фикс, поставленный ПРОТИВ обнуления счётчиков, сам открыл
    // бы дорогу к обнулению. Найдено ревью (итерация 1).
    // ★РАЗБОР ОДИН НА ВСЕХ (правка ревью, итерация 7): тот же `ParseRecordUid`
    // читает личность записи и в перестройках индексов, иначе пол счётчика и
    // индекс расходились бы в том, какой файл считать записью.
    const auto parsed = ParseRecordUid(entryPath.stem().string());
    if (not parsed)
      continue;

    scan.highest = std::max(scan.highest, static_cast<uint32_t>(*parsed));
  }

  return scan;
}

} // anon namespace

void server::FileDataSource::Initialize(const std::filesystem::path& path)
{
  // ★СВЕРКА ИДЁТ С ТЕМ, ЧЕМ ЧЛЕН РЕАЛЬНО ИНИЦИАЛИЗИРУЕТСЯ (правка ревью,
  // итерация 2). Прежняя редакция сверяла гейт с ЛИТЕРАЛОМ, написанным в самом
  // `static_assert`, а инициализатор члена нёс свой собственный литерал: правка
  // `{64}` -> `{32}` проходила компиляцию молча, то есть защита от расхождения
  // была объявлена, но не работала. Теперь сверяется именно та константа,
  // которой инициализируется потолок.
  //
  // Утверждения стоят В ТЕЛЕ МЕТОДА, а не в области имён файла, потому что
  // константы приватные: членская функция имеет к ним доступ, свободная — нет.
  static_assert(server::util::kMaxStoredNameBytes == kCharacterNameCeilingFloor,
    "the character name ceiling floor has drifted from the name guard's bound");
  static_assert(server::util::kMaxLoginNameBytes == kLoginNameCeilingFloor,
    "the login name ceiling floor has drifted from the name guard's bound");

  // ★ПРИЁМНИК ПРЕДУПРЕЖДЕНИЙ ФАЙЛОВОГО ПОМОЩНИКА СТАВИТСЯ ЗДЕСЬ (LOA-fix R73-5,
  // ревью 4). `AtomicFile.hpp` сознательно не тянет spdlog (см. его заголовок),
  // поэтому редкие жалобы — отвергнутая ссылка, не усыновлённый владелец,
  // сужение поздно пришедшего файла — выходят через указатель на функцию.
  // Установка ДО первого обращения к диску: иначе первое же сообщение ушло бы в
  // тишину, а «сузили молча» неотличимо от «не сузили».
  server::util::SetFileWarningSink(
    [](const std::string_view message)
    {
      server::util::QuietLogWarn("{}", message);
    });

  _dataPath = path;
  _metaFilePath = _dataPath;

  const auto prepareDataPath = [this](const std::filesystem::path& folder)
  {
    const auto path = _dataPath / folder;
    // ★БЕЗ ПЕРЕХОДА ПО ССЫЛКАМ (правка ревью, итерация 6): `create_directories`
    // разрешала путь именами, и ссылка на месте промежуточного каталога уводила
    // создание — а с ним и все записи и удаления — за пределы `data/`.
    if (not server::util::CreateManagedDirectories(path))
    {
      throw std::runtime_error(
        std::format(
          "Data directory '{}' could not be created without following a "
          "symbolic link", server::util::LogPath(path)));
    }
    return path;
  };

  // Prepare the data paths.
  // ★РЕЖИМ ПРИНУДИТЕЛЬНО СУЖАЕТСЯ РОВНО У ОДНОГО КАТАЛОГА — `users`: только в
  // нём лежат `passwordHash` и `passwordSalt` (StoreUser ниже). Остальные
  // ПЯТНАДЦАТЬ создаются как создавались и их режим НЕ ТРОГАЕТСЯ ВОВСЕ.
  //
  // ★ПРАВКА РЕВЬЮ (итерация 1): прежняя редакция ставила всем пятнадцати ТОЧНЫЙ
  // 0755. Это не «оставить как было», а НАЗНАЧИТЬ: у оператора, который сузил
  // `mails` или `infractions` вручную либо поставил на каталог setgid, каждый
  // старт сервера молча возвращал бы права ШИРЕ. Правка, взявшаяся сужать, не
  // имеет права расширять.
  _userDataPath = prepareDataPath("users");
  _infractionDataPath = prepareDataPath("infractions");
  _characterDataPath = prepareDataPath("characters");
  _itemDataPath = prepareDataPath("characters/equipment/items");
  _horseDataPath = prepareDataPath("characters/equipment/horses");
  _storageItemPath = prepareDataPath("storage");
  _eggDataPath = prepareDataPath("eggs");
  _petDataPath = prepareDataPath("pets");
  _housingDataPath = prepareDataPath("housing");
  _guildDataPath = prepareDataPath("guilds");
  _settingsDataPath = prepareDataPath("settings");
  _dailyQuestGroupDataPath = prepareDataPath("dailyQuestGroups");
  _mailDataPath = prepareDataPath("mails");
  _questDataPath = prepareDataPath("quests");
  _stallionDataPath = prepareDataPath("stallions");
  _rewardDataPath = prepareDataPath("rewards");

  // ★СУЖЕНИЕ СУЩЕСТВУЮЩЕГО, а не только создание с правами: `data/users` на
  // проде создан давно и стоит 0755 — создание его не тронет (LOA-fix R73-2).
  if (not server::util::EnsureDirectoryMode(_userDataPath, 0700))
  {
    server::util::QuietLogError(
      "Data directory '{}': could not restrict the directory permissions to {:o}",
      server::util::LogPath(_userDataPath), 0700);
  }

  // ★И САМИ ИНОДЫ, А НЕ ТОЛЬКО КАТАЛОГ (LOA-fix R73-2b, правка ревью 1).
  // Обязательный `FileSensitivity` чинит режим НА ПЕРВОЙ ЗАПИСИ, но аккаунт, в
  // который сервер больше не пишет (игрок не заходит), первой записи не
  // дождётся никогда и остался бы 0644 бессрочно. Один проход на старте, без
  // единой перезаписи содержимого.
  //
  // ★ЭТОТ ПРОХОД НЕ ЛИШНИЙ ПРИ ТОМ, ЧТО ПЕРЕСТРОЙКА ИНДЕКСА НИЖЕ СУЖАЕТ ТЕ ЖЕ
  // ФАЙЛЫ (пояснение к правке ревью, итерация 4). Сужают оба, но ВЕРДИКТ выносит
  // только этот: перестройка живёт и на пути запроса, поэтому она обязана
  // ЖУРНАЛИРОВАТЬ отказ, а не бросать — иначе staff-команда рвалась бы вместо
  // ответа. Единственное место, где «хеш пароля не удалось защитить» может
  // КОГО-ТО ОСТАНОВИТЬ, — старт; здесь оно и стоит. Убрать этот проход значило
  // бы оставить сужение без единого читателя вердикта, то есть вернуть
  // инвариант, который умеет не выполниться и никого не остановить.
  const auto hardening =
    server::util::HardenSecretFilesInDirectory(_userDataPath);
  if (hardening.narrowed > 0)
  {
    server::util::QuietLogWarn(
      "Account files in '{}': {} of {} examined were group/other-readable and "
      "were narrowed to owner-only",
      server::util::LogPath(_userDataPath), hardening.narrowed, hardening.examined);
  }

  // ★ССЫЛКА В КАТАЛОГЕ АККАУНТОВ НАЗЫВАЕТСЯ ВСЛУХ, А НЕ ПРОПУСКАЕТСЯ (правка
  // ревью, итерация 4). Прежде проход рапортовал полный успех, ни разу не
  // тронув `Alice.json`, ставший ссылкой на файл 0644, — и это было ХУЖЕ
  // отсутствия прохода: отчёт утверждал ровно то, чего не произошло.
  //
  // ★ЖУРНАЛ, А НЕ ОТКАЗ СТАРТОВАТЬ, и это не смягчение политики. Ссылка теперь
  // отвергается ВСЕМИ потребителями: вход по ней не аутентифицирует
  // (`ReadManagedFile` даёт `ELOOP`), индекс её не заводит, запись сквозь неё
  // бросает. То есть аккаунт УЖЕ недоступен — падать сверх этого значило бы
  // отнять сервер у всех остальных из-за мусора, положенного рядом.
  if (hardening.refusedLinks > 0)
  {
    server::util::QuietLogWarn(
      "Account files in '{}': {} entry(ies) are symbolic links and were refused; "
      "they are neither hardened, indexed nor authenticated from",
      server::util::LogPath(_userDataPath), hardening.refusedLinks);
  }

  // ★ОТКАЗ СУЖЕНИЯ ОСТАНАВЛИВАЕТ СТАРТ (правка ревью, итерация 2). Прежняя
  // редакция считала отказы и печатала строку, после чего сервер начинал
  // обслуживать игроков — то есть при `EPERM`/`EROFS`/`EIO` хеш пароля
  // оставался читаемым для group/other, а корневой инвариант раунда («секрет не
  // лежит в файле, доступном кому-то кроме владельца») держался бы только на
  // том, что кто-то прочитает лог. Инвариант, который умеет не выполниться и
  // никого не остановить, — не инвариант.
  //
  // ★НЕЗАВЕРШЁННЫЙ ОБХОД ТОЖЕ ФАТАЛЕН. `incomplete` означает, что часть файлов
  // мы НЕ ОСМОТРЕЛИ: «0 отказов» тогда говорит не «всё чисто», а «мы не знаем».
  // Отличить эти два случая обязан код, а не читатель лога.
  if (hardening.failed > 0 || hardening.incomplete)
  {
    server::util::QuietLogError(
      "Account files in '{}': {} of {} examined could not be secured{}; "
      "refusing to start with password hashes readable beyond their owner",
      server::util::LogPath(_userDataPath), hardening.failed, hardening.examined,
      hardening.incomplete ? ", and the directory scan did not finish" : "");

    throw std::runtime_error(
      std::format(
        "Account files in '{}' could not be restricted to owner-only "
        "({} failed, {} examined, scan {})",
        server::util::LogPath(_userDataPath), hardening.failed, hardening.examined,
        hardening.incomplete ? "incomplete" : "complete"));
  }

  // Read the meta-data file and parse the sequential UIDs.
  const std::filesystem::path metaFilePath = ProduceDataFilePath(
    _metaFilePath, "meta");
  // LOA-fix (R58-5, round58, backlog #175): уборка недописанных временных
  // файлов от прерванных сохранений. Без неё осиротевший `X.json.tmp` копился бы
  // и попадал под обходы каталогов.
  server::util::SweepStaleTemporaries(_dataPath);

  nlohmann::json meta = nlohmann::json::object();

  // ★ЧЕРЕЗ ТОТ ЖЕ ЕДИНСТВЕННЫЙ ВХОД В ЧТЕНИЕ (правка ревью, итерация 4): мета
  // лежит под `data/` и подчиняется той же политике, что остальные записи —
  // ссылка на него не читается.
  const auto metaRead = server::util::ReadManagedFile(
    metaFilePath, server::util::FileSensitivity::Public);
  if (metaRead.status != server::util::ManagedReadStatus::Ok)
  {
    // ★РАНЬШЕ ЗДЕСЬ БЫЛ ТИХИЙ `return` — без единой строки в логе. Он и делал
    // потерю мета-файла беззвучной катастрофой: все счётчики оставались нулями,
    // следующий персонаж получал uid 1 и затирал файл старейшего игрока.
    server::util::QuietLogError(
      "Sequential uid metadata '{}' is missing. Uid counters will be derived from "
      "the data on disk", server::util::LogPath(metaFilePath));
  }
  else
  {
    try
    {
      meta = nlohmann::json::parse(metaRead.content);
    }
    catch (const std::exception& x)
    {
      // Битый мета тоже не повод падать: пол из данных сильнее любого файла.
      server::util::QuietLogError(
        "Sequential uid metadata '{}' is unreadable ({}). Uid counters will be "
        "derived from the data on disk", server::util::LogPath(metaFilePath), x.what());
      meta = nlohmann::json::object();
    }
  }

  _infractionSequentialUid = meta.value("infractionSequentialUid", uint32_t{0});
  _characterSequentialUid = meta.value("characterSequentialUid", uint32_t{0});
  _equipmentSequentialUid = meta.value("equipmentSequentialUid", uint32_t{0});
  _storageItemSequentialUid = meta.value("storageItemSequentialUid", uint32_t{0});
  _eggSequentialUid = meta.value("eggSequentialUid", uint32_t{0});
  _petSequentialUid = meta.value("petSequentialUid", uint32_t{0});
  _housingSequentialUid = meta.value("housingSequentialUid", uint32_t{0});
  _guildSequentialId = meta.value("guildSequentialId", uint32_t{0});
  _settingsSequentialId = meta.value("settingsSequentialId", uint32_t{0});
  _dailyQuestGroupSequentialId = meta.value("dailyQuestGroupSequentialId", uint32_t{0});
  _mailSequentialId = meta.value("mailSequentialId", uint32_t{0});
  _questSequentialId = meta.value("questSequentialId", uint32_t{0});
  _stallionSequentialUid = meta.value("stallionSequentialUid", uint32_t{0});
  _rewardSequentialUid = meta.value("rewardSequentialUid", uint32_t{0});

  // ★ПОЛ СЧЁТЧИКОВ ПРИМЕНЯЕТСЯ ВСЕГДА, А НЕ ТОЛЬКО ПРИ СБОЕ РАЗБОРА.
  // В этом и смысл: мета может быть не только потерян, но и устарел, откачен
  // вместе с бэкапом или отстать после ручной правки. Пол из фактических имён
  // файлов обезвреживает ВЕСЬ класс — выдать занятый uid становится нельзя.
  const auto raiseFloor = [](std::atomic<uint32_t>& counter, const uint32_t observed)
  {
    if (observed > counter.load())
      counter.store(observed);
  };

  // ★НЕПОЛНЫЙ СНИМОК КАТАЛОГА НЕ ИМЕЕТ ПРАВА СТАТЬ ПОЛОМ (правка ревью,
  // итерация 3). Оборванный обход отдаёт МЕНЬШИЙ максимум, и «пол применён»
  // тогда означает «пол занижен»: `meta` потерян, обход не дошёл до
  // `characters/100.json`, счётчик восстановлен как 99 — и следующий персонаж
  // получает uid 100, а `WriteFileAtomically` кладёт его ПОВЕРХ живого файла.
  // Ровно тот дефект, против которого пол и заведён, только с другой стороны.
  // Поэтому неполнота фатальна: не выдать uid лучше, чем выдать занятый.
  const auto uidFloor = [](const std::filesystem::path& root)
  {
    const auto scan = HighestUidInDirectory(root);
    if (scan.incomplete)
    {
      server::util::QuietLogError(
        "Uid floor: the scan of '{}' did not finish; the highest uid on disk is "
        "unknown, so the next entity could be handed an occupied uid and "
        "overwrite a live record",
        server::util::LogPath(root));

      throw std::runtime_error(
        std::format(
          "Uid floor scan of '{}' did not finish", server::util::LogPath(root)));
    }
    return scan.highest;
  };

  raiseFloor(_infractionSequentialUid, uidFloor(_infractionDataPath));
  raiseFloor(_characterSequentialUid, uidFloor(_characterDataPath));
  // ★Один счётчик на ДВА каталога — лошади и предметы делят нумерацию.
  raiseFloor(_equipmentSequentialUid, std::max(
    uidFloor(_horseDataPath), uidFloor(_itemDataPath)));
  raiseFloor(_storageItemSequentialUid, uidFloor(_storageItemPath));
  raiseFloor(_eggSequentialUid, uidFloor(_eggDataPath));
  raiseFloor(_petSequentialUid, uidFloor(_petDataPath));
  raiseFloor(_housingSequentialUid, uidFloor(_housingDataPath));
  raiseFloor(_guildSequentialId, uidFloor(_guildDataPath));
  raiseFloor(_settingsSequentialId, uidFloor(_settingsDataPath));
  raiseFloor(_dailyQuestGroupSequentialId,
    uidFloor(_dailyQuestGroupDataPath));
  raiseFloor(_mailSequentialId, uidFloor(_mailDataPath));
  raiseFloor(_questSequentialId, uidFloor(_questDataPath));
  raiseFloor(_stallionSequentialUid, uidFloor(_stallionDataPath));
  raiseFloor(_rewardSequentialUid, uidFloor(_rewardDataPath));

  // LOA-fix (R73-4, #130-C8): индекс имён строится ОДИН раз, здесь. После
  // `raiseFloor` намеренно: обход каталога персонажей уже прогрет, и порядок
  // «сперва счётчики, потом индекс» держит стартовые инварианты в одном месте.
  RebuildCharacterNameIndex();
  RebuildUserNameIndex();
  // ★И ИНДЕКС ГИЛЬДИЙ (правка ревью, итерация 6): пока его не было, проверка
  // уникальности имени обходила каталог гильдий на каждый пакет создания.
  RebuildGuildNameIndex();
}

void server::FileDataSource::Terminate()
{
  SaveMetadata();
  // ★ПРОВЕРКА, ЧЕЙ ВЕРДИКТ КТО-ТО ЧИТАЕТ. Структурный гейт имени молчит на
  // каждом пакете НАМЕРЕННО (иначе клиент сам себе делает поток строк в лог —
  // ровно дефект R57: HandleRaceUserPos дал 15 350 строк/час). Но гейт, о
  // котором нельзя узнать, работал ли он, — это не гейт; поэтому счётчик один
  // и печатается один раз.
  server::util::QuietLogInfo(
    "Name lookup guard: rejected {} out-of-class name lookups",
    _rejectedNameLookups.load(std::memory_order::relaxed));
}

void server::FileDataSource::SaveMetadata()
{
  // dirty fix to make this thread safe
  static std::mutex dirty;
  std::scoped_lock fix(dirty);

  const std::filesystem::path metaFilePath = ProduceDataFilePath(
    _metaFilePath, "meta");

  nlohmann::json meta;
  meta["infractionSequentialUid"] = _infractionSequentialUid.load();
  meta["characterSequentialUid"] = _characterSequentialUid.load();
  meta["equipmentSequentialUid"] = _equipmentSequentialUid.load();
  meta["storageItemSequentialUid"] = _storageItemSequentialUid.load();
  meta["eggSequentialUid"] = _eggSequentialUid.load();
  meta["petSequentialUid"] = _petSequentialUid.load();
  meta["housingSequentialUid"] = _housingSequentialUid.load();
  meta["guildSequentialId"] = _guildSequentialId.load();
  meta["settingsSequentialId"] = _settingsSequentialId.load();
  meta["dailyQuestGroupSequentialId"] = _dailyQuestGroupSequentialId.load();
  meta["mailSequentialId"] = _mailSequentialId.load();
  meta["questSequentialId"] = _questSequentialId.load();
  meta["stallionSequentialUid"] = _stallionSequentialUid.load();
  meta["rewardSequentialUid"] = _rewardSequentialUid.load();

  // LOA-fix (R58-3b, round58, backlog #175): мета-файл переписывается на
  // КАЖДОМ создании сущности, то есть окно обрезания открыто постоянно. Пустой
  // мета не даёт серверу СТАРТОВАТЬ, а отсутствующий молча обнуляет все
  // четырнадцать счётчиков — и следующий персонаж получает uid 1, затирая файл
  // старейшего игрока. Пишем атомарно; контракт «не бросать» сохраняем, но
  // молчать больше нельзя.
  try
  {
    server::util::WriteFileAtomically(
      metaFilePath, meta.dump(2), "Meta file", server::util::FileSensitivity::Public);
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError(
      "Failed to save the sequential uid metadata: {}. New uids may collide with "
      "existing records after a restart", x.what());
  }
}

void server::FileDataSource::CreateUser(data::User& user)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _userDataPath, user.name());

}

void server::FileDataSource::RetrieveUser(const std::string_view& name, data::User& user)
{
  user.name = std::string(name);

  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _userDataPath, user.name());

  const auto json = ReadManagedJson(
    dataFilePath, "User file", server::util::FileSensitivity::Secret);
  user.name = json.value("name", std::string{});
  user.token = json.value("token", std::string{});
  user.characterUid = json.value("characterUid", data::Uid{});
  user.infractions = json.value("infractions", std::vector<data::Uid>{});
  user.lastSeenOnline = data::Clock::time_point(std::chrono::seconds(
    json.value("lastSeenOnline", int64_t(0))));
  // LOA-fix (#18c): хеш+соль пароля. Нет ключа → "" (старые файлы без миграции).
  user.passwordHash = json.value("passwordHash", std::string{});
  user.passwordSalt = json.value("passwordSalt", std::string{});
}

void server::FileDataSource::StoreUser(const std::string_view&, const data::User& user)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _userDataPath, user.name());

  nlohmann::json json;
  json["name"] = user.name();
  json["token"] = user.token();
  json["characterUid"] = user.characterUid();
  json["infractions"] = user.infractions();
  json["lastSeenOnline"] = std::chrono::ceil<std::chrono::seconds>(
    user.lastSeenOnline().time_since_epoch()).count();
  // LOA-fix (#18c): сохраняем хеш+соль пароля, иначе logout затёр бы их.
  json["passwordHash"] = user.passwordHash();
  json["passwordSalt"] = user.passwordSalt();

  // ★ЕДИНСТВЕННЫЙ ФАЙЛ, В КОТОРЫЙ ЭТОТ КЛАСС ПИШЕТ ХЕШ ПАРОЛЯ (см. выше,
  // `passwordHash`/`passwordSalt`).
  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "User file",
    server::util::FileSensitivity::Secret);

  // ★ИНДЕКС ПОСЛЕ УСПЕШНОЙ ЗАПИСИ, как и у персонажей: бросок оставляет диск в
  // прежнем состоянии, и индекс обязан остаться согласованным с диском.
  IndexUserName(user.name());
}

bool server::FileDataSource::IsUserNameUnique(const std::string_view& name)
{
  // ★СНАЧАЛА КЛАСС ИМЕНИ, ПОТОМ ЧТО-ЛИБО ЕЩЁ. Прежняя редакция СОБИРАЛА
  // РЕГУЛЯРНОЕ ВЫРАЖЕНИЕ из присланного имени (`std::format("{}.*", name)`) до
  // всякой проверки. Имя вида `(a{200}){200}` разворачивает автомат libstdc++ в
  // миллионы состояний ещё на КОНСТРУКЦИИ, а имя `[` бросает `regex_error`
  // наружу — то есть строка `[error]` на КАЖДЫЙ пакет.
  //
  // Потолок берётся из ИНДЕКСА, а не из константы (правка ревью, итерация 1):
  // аккаунт, заведённый до появления проверки #18b, может быть длиннее
  // сегодняшних 48 байт, и отбить его на входе значило бы сделать реального
  // игрока неадресуемым для staff-команды.
  //
  // ★ГЕЙТ ПОИСКА — СТРУКТУРНЫЙ, А НЕ ALLOWLIST АУТЕНТИФИКАЦИИ (правка ревью,
  // итерация 3). Прежняя редакция звала здесь `IsLoginNameSafe`, то есть
  // сегодняшний класс РЕГИСТРАЦИИ `[A-Za-z0-9_-]`. Но индекс строится из имён
  // ФАЙЛОВ и никакого класса не требует: `data/users/john.doe.json`, заведённый
  // до появления проверки #18b (или скриптом), попадает в индекс как
  // `john.doe` — и тут же становится неадресуемым, потому что гейт отбивает
  // точку ДО обращения к индексу. `//infraction list john.doe` отвечал бы
  // «пользователя нет» про существующего пользователя, хотя обход, который мы
  // заменили индексом, его находил. Гейт, который строже индекса, который он
  // охраняет, отнимает путь успеха у честного администратора.
  //
  // Поэтому здесь стоит ТОТ ЖЕ структурный гейт, что и у поиска персонажа:
  // непустое, не длиннее потолка, без управляющих байтов и без разделителей
  // пути. Он по-прежнему отбивает всё, ради чего гейт заводился (регулярка из
  // имени, 8 КБ с провода, `../`), но не выдумывает класса символов, которого
  // на диске нет. Строгий allowlist остаётся там, где он и уместен, — в
  // `LocalAuthenticationBackend` (вход и регистрация): это правило о том, какие
  // имена МОЖНО ЗАВЕСТИ, а не о том, какие УЖЕ лежат.
  if (not server::util::IsStorableNameShaped(
    name, _loginNameCeiling.load(std::memory_order::relaxed)))
  {
    _rejectedNameLookups.fetch_add(1, std::memory_order::relaxed);
    // Имя, которого не может существовать, «уникально»: вызывающий (staff-
    // команда `//infraction`, ChatSystem.cpp) ответит «пользователя нет», а не
    // наоборот. Регистрации через этот путь нет — `LocalAuthenticationBackend`
    // проверяет имя сам.
    return true;
  }

  // ★ИНДЕКС ВМЕСТО ОБХОДА КАТАЛОГА (LOA-fix R73-4b, правка ревью 1). Снять
  // регулярку было половиной дела: обход `data/users` на КАЖДУЮ staff-команду
  // оставлял стоимость пакета линейной по числу аккаунтов, то есть заявленный
  // класс «имя с провода не вызывает обхода файловой системы» оставался
  // открытым. Сравнение по-прежнему ASCII-регистронезависимое — ключ индекса
  // и есть имя в нижнем ASCII-регистре, поэтому смысл ответа не изменился.
  const auto key = server::util::AsciiLowerKey(name);

  bool present = false;
  {
    const std::shared_lock indexLock(_userNameIndexMutex);
    present = _userNameKeys.contains(key);
  }

  // ★ПРОМАХ ПЕРЕСПРАШИВАЕТ ИНДЕКС, А НЕ ФАЙЛОВУЮ СИСТЕМУ ПО ТОЧНОМУ ИМЕНИ
  // (правка ревью, итерация 2). Прежняя редакция добирала промах одним
  // `stat` пути `<name>.json` — и этим МОЛЧА теряла регистронезависимость:
  // индекс сравнивает по ASCII-нижнему регистру (и прежний обход сравнивал
  // регуляркой с `icase`), а `stat` на ext4 регистрозависим. Аккаунт
  // `Alice.json`, заведённый скриптом рядом с работающим сервером, на команду
  // `//infraction list alice` отвечал бы «такого игрока нет», хотя обход,
  // который мы заменили, его находил. Это потеря пути успеха у честного
  // администратора — ровно то, чего замена обхода индексом делать не должна.
  //
  // ★И ЭТО НЕ ВОЗВРАТ ОБХОДА НА ПАКЕТ. Перестройка ограничена и по поводу, и по
  // частоте: не чаще одного раза в `kUserIndexReconcileGap` (см. ниже), сколько
  // бы промахов ни пришло и сколько бы потоков ни промахнулось одновременно.
  //
  // ★СВЕРКА ИДЁТ И ПРИ ПОПАДАНИИ, А НЕ ТОЛЬКО ПРИ ПРОМАХЕ (правка ревью,
  // итерация 4). Прежняя редакция возвращала «имя занято» прямо из индекса и
  // до диска не доходила ВООБЩЕ — то есть УСТАРЕВШАЯ ПОЛОЖИТЕЛЬНАЯ запись жила
  // вечно: `Alice.json` переименовали рядом с работающим сервером, а
  // `//infraction list Alice` продолжал отвечать «запись временно недоступна»
  // до перезапуска, потому что путь к сверке проходил только через промах.
  // Ограничение частоты — потолок в `kUserIndexReconcileGap` и пол в
  // `kUserIndexStaleAfter` — общее для обоих исходов, поэтому единый путь
  // ничего не удорожает: он ровно тот же обход, только теперь он достижим и
  // тогда, когда индекс ошибается в другую сторону.
  if (RefreshUserNameIndexIfDirectoryChanged())
  {
    const std::shared_lock indexLock(_userNameIndexMutex);
    present = _userNameKeys.contains(key);
  }

  // ★ИНДЕКС, О КОТОРОМ ИЗВЕСТНО, ЧТО ОН НЕПОЛОН, НЕ ОТВЕЧАЕТ «СВОБОДНО»
  // (правка ревью, итерация 7).
  //
  // Отпечаток объявляется недействительным ровно тогда, когда обход не дошёл до
  // конца, файл не удалось сузить или регистрация имени не удалась. Прежде
  // такой индекс всё равно отвечал по своему содержимому, а пол частоты
  // (`kUserIndexReconcileGap`) стоит ВЫШЕ проверки действительности — то есть
  // до пяти секунд промах читался как «такого аккаунта нет», хотя мы знаем, что
  // видели не всё. Пол не трогаем (он и держит стоимость пакета), меняем
  // ОТВЕТ: «не знаю» — это «занято», а не «свободно». Регистрацию это не
  // ломает: её проверяет `LocalAuthenticationBackend` по самому файлу.
  if (not present && not _userIndexStampValid.load(std::memory_order::relaxed))
    return false;

  return not present;
}

bool server::FileDataSource::NeedsUserIndexReconcile(
  const std::filesystem::file_time_type stamp,
  const bool stampUnreadable,
  const std::chrono::steady_clock::time_point now) const
{
  // Вызывается ТОЛЬКО с удержанным `_userNameIndexMutex` (любым из двух
  // режимов): читает три поля индекса и ничего не блокирует сам.

  // ★ПОТОЛОК ЧАСТОТЫ — ПЕРВЫЙ, И ЭТО ГЛАВНАЯ ПРАВКА (ревью, итерация 3).
  // Перестройка стоит O(число аккаунтов), а СВОЯ ЖЕ запись аккаунта меняет
  // mtime каталога: staff-клиент, чередуя безобидное `//infraction remove <имя>
  // 0` (ChatSystem.cpp:1345 сохраняет запись даже когда удалять нечего) с
  // запросом отсутствующего имени, заказывал по полному обходу на пару. Здесь
  // же стоит и КОАЛЕСЦЕНЦИЯ: тот же вопрос задаётся повторно под эксклюзивным
  // замком, и поток, дождавшийся чужой перестройки, видит свежий `_lastScan` и
  // не перестраивает второй раз.
  if (now - _userIndexLastScan < kUserIndexReconcileGap)
    return false;

  // ★А ЭТО — ПОЛ ЧАСТОТЫ, И ОН ЗАКРЫВАЕТ РАВЕНСТВО ОТПЕЧАТКОВ (ревью,
  // итерация 3, WARN о коллизиях mtime). Отпечаток каталога НЕ является
  // свободным от коллизий признаком изменения: на файловой системе с грубым
  // разрешением времени, а равно после восстановления из копии, которая
  // возвращает каталогу прежний mtime, только что появившийся `Alice.json`
  // остаётся под старым отпечатком — и без принудительной сверки не нашёлся бы
  // до перезапуска. Раз в `kUserIndexStaleAfter` промах сверяется с диском
  // независимо от отпечатка, поэтому «никогда» превращается в «в течение
  // минуты».
  if (now - _userIndexLastScan >= kUserIndexStaleAfter)
    return true;

  // Отпечаток совпал — каталог не менялся с последней ПОЛНОЙ перестройки,
  // значит промах индекса и есть ответ «такого аккаунта нет».
  return stampUnreadable
    || not _userIndexStampValid.load(std::memory_order::relaxed)
    || stamp != _userIndexDirectoryStamp;
}

bool server::FileDataSource::RefreshUserNameIndexIfDirectoryChanged()
{
  const auto now = std::chrono::steady_clock::now();

  std::error_code error;
  const auto stamp = std::filesystem::last_write_time(_userDataPath, error);

  {
    const std::shared_lock indexLock(_userNameIndexMutex);
    if (not NeedsUserIndexReconcile(stamp, static_cast<bool>(error), now))
      return false;
  }

  // ★ОБХОД ФАЙЛОВОЙ СИСТЕМЫ ИДЁТ ПОД СВОИМ ЗАМКОМ, А НЕ ПОД ЗАМКОМ ИНДЕКСА
  // (правка ревью, итерация 5).
  //
  // Прежняя редакция держала ЭКСКЛЮЗИВНЫЙ `_userNameIndexMutex` всё время
  // обхода: одна staff-команда после порога устаревания открывала каталог,
  // делала `open`/`fstat`/`close` на каждом аккаунте — а `IndexUserName`, то
  // есть путь сохранения `DataDirector`, стоял в очереди за ней. На живом
  // шарде в 13 аккаунтов это доли миллисекунды, но стоимость держится не
  // числом аккаунтов сегодня, а формой: замок, под которым делают ввод-вывод,
  // рано или поздно останавливает того, кто просто пишет в память.
  //
  // Отдельный `_userIndexRebuildMutex` даёт ровно два свойства: обход в один
  // момент времени ровно один (коалесценция сохранена) и индекс всё это время
  // ЧИТАЕМ И ПИШЕМ — под замок индекса уходит только публикация готового
  // набора, то есть один `move` и четыре присваивания.
  const std::unique_lock rebuildGuard(_userIndexRebuildMutex);

  // ★ПОВТОРНАЯ ПРОВЕРКА ПОД ЗАМКОМ ПЕРЕСТРОЙКИ — ЭТО И ЕСТЬ КОАЛЕСЦЕНЦИЯ
  // (правка ревью, итерация 3). Прежняя редакция отпускала общий замок и звала
  // перестройку безусловно: несколько потоков, промахнувшихся одновременно,
  // проходили проверку все, а потом ПО ОЧЕРЕДИ делали по полному обходу.
  // Вопрос задаётся тем же `now`, что и снаружи, поэтому перестройка, успевшая
  // завершиться после нашего входа, гарантированно закрывает нам дорогу.
  {
    const std::shared_lock indexLock(_userNameIndexMutex);
    if (not NeedsUserIndexReconcile(stamp, static_cast<bool>(error), now))
      return false;
  }

  RebuildUserNameIndexUnderRebuildGuard();
  return true;
}

void server::FileDataSource::RebuildUserNameIndex()
{
  const std::unique_lock rebuildGuard(_userIndexRebuildMutex);
  RebuildUserNameIndexUnderRebuildGuard();
}

void server::FileDataSource::RebuildUserNameIndexUnderRebuildGuard()
{
  RaiseNameCeiling(_loginNameCeiling, server::util::kMaxLoginNameBytes);

  // ★ЗАЯВКА НА ПРОПАЖУ: пока идёт обход, `IndexUserName` вправе внести имя,
  // которого в снимке каталога ещё не было. Публикация набора, снятого ДО этой
  // регистрации, потеряла бы её до следующей сверки — то есть замена «строим
  // под замком» на «строим снаружи» стоила бы только что зарегистрированному
  // игроку ответа `//infraction list`. Поэтому такие имена собираются отдельно
  // и вливаются в набор при публикации.
  {
    const std::unique_lock indexLock(_userNameIndexMutex);
    _userNamesAddedDuringScan.clear();
    _userIndexScanInFlight = true;
  }

  // ★ФЛАГ ОБХОДА СНИМАЕТСЯ НА ЛЮБОМ ВЫХОДЕ, А НЕ ТОЛЬКО НА УСПЕШНОЙ ПУБЛИКАЦИИ
  // (правка ревью, итерация 6).
  //
  // Между установкой флага и публикацией стоят `push_back`, преобразования
  // путей и вставки в множество — каждая умеет бросить `bad_alloc`. Прежняя
  // редакция снимала флаг ТОЛЬКО в блоке публикации, поэтому бросок посреди
  // обхода оставлял `_userIndexScanInFlight` истинным НАВСЕГДА: каждое
  // последующее сохранение аккаунта продолжало наполнять
  // `_userNamesAddedDuringScan`, который никто уже не сливал и не чистил, —
  // то есть утечка, растущая ровно на пути, который раунд удешевлял.
  // Обязательство, которое умеет не выполниться, — не обязательство.
  struct ScanFlagGuard
  {
    server::FileDataSource* owner;
    bool armed = true;

    ~ScanFlagGuard()
    {
      if (not armed)
        return;
      try
      {
        const std::unique_lock indexLock(owner->_userNameIndexMutex);
        owner->_userNamesAddedDuringScan.clear();
        owner->_userIndexScanInFlight = false;
      }
      catch (...)
      {
        // Деструктор не имеет права бросить: отказ самого замка — это уже
        // остановка процесса другими средствами, а не наша.
      }
    }
  } scanFlagGuard{this};

  // ★ОТПЕЧАТОК СНИМАЕТСЯ ДО ОБХОДА, А НЕ ПОСЛЕ. Файл, положенный рядом ВО ВРЕМЯ
  // обхода, обязан оставить отпечаток «устаревшим», иначе он потерялся бы до
  // перезапуска. Снимок «до» ошибается только в безопасную сторону — лишняя
  // перестройка, а не пропущенный аккаунт.
  std::error_code stampError;
  const auto stamp = std::filesystem::last_write_time(_userDataPath, stampError);

  const auto listing = server::util::ListRegularFiles(_userDataPath);

  // ★ПОЗДНО ПРИШЕДШИЙ ФАЙЛ СУЖАЕТСЯ ДО ТОГО, КАК ПОПАДЁТ В ИНДЕКС (LOA-fix
  // R73-6, правка ревью, итерация 4).
  //
  // Стартовый проход `HardenSecretFilesInDirectory` — это СНИМОК. Штатный путь
  // завести аккаунт рядом с работающим сервером (помощник переименовывает
  // готовый `Alice.json` в `data/users`) кладёт файл с обычным umask, то есть
  // 0644, и до перезапуска ИЛИ до следующего сохранения этого аккаунта хеш
  // пароля лежал читаемым для group/other — а сервер тем временем спокойно им
  // пользовался. Перестройка индекса и есть тот момент, когда файл впервые
  // становится ВИДЕН серверу; сузить его позже, чем внести в индекс, значило бы
  // сделать запись используемой раньше, чем защищённой.
  //
  // ★ТОТ ЖЕ СПИСОК, А НЕ ВТОРОЙ ОБХОД: между двумя независимыми обходами лежало
  // бы окно, в котором сужен один набор файлов, а проиндексирован другой.
  const auto hardening = server::util::HardenSecretFiles(listing);
  if (hardening.narrowed > 0)
  {
    server::util::QuietLogWarn(
      "Account name index: {} of {} account files in '{}' were group/other-"
      "readable and were narrowed to owner-only before being indexed",
      hardening.narrowed, hardening.examined, server::util::LogPath(_userDataPath));
  }
  if (hardening.failed > 0)
  {
    // ★ЖУРНАЛ, А НЕ БРОСОК — по той же причине, что и у неполноты ниже: эта
    // перестройка живёт на пути запроса, и бросок отсюда рвал бы staff-команду.
    // На СТАРТЕ тот же отказ фатален (`Initialize`), поэтому «не сузили» не
    // остаётся без остановки там, где остановка возможна.
    server::util::QuietLogError(
      "Account name index: {} account file(s) in '{}' could not be narrowed to "
      "owner-only; they are refused, not indexed, and cannot be authenticated "
      "from until they are secured",
      hardening.failed, server::util::LogPath(_userDataPath));
  }
  if (hardening.refusedLinks > 0)
  {
    server::util::QuietLogWarn(
      "Account name index: {} entry(ies) in '{}' are symbolic links and were "
      "refused; they are not indexed and cannot be authenticated from",
      hardening.refusedLinks, server::util::LogPath(_userDataPath));
  }

  if (listing.incomplete || hardening.incomplete)
  {
    // ★ЗДЕСЬ ЖУРНАЛ, А НЕ ОТКАЗ СТАРТОВАТЬ — И ЭТО НЕ НЕПОСЛЕДОВАТЕЛЬНОСТЬ
    // (пояснение к правке ревью, итерация 3). Каталог `data/users` на старте
    // УЖЕ доказан полным: `HardenSecretFilesInDirectory` идёт по нему тем же
    // обходом раньше в `Initialize` и при неполноте бросает. А в рантайме эта
    // перестройка живёт на пути запроса: бросок отсюда рвал бы staff-команду
    // вместо ответа. Неполнота при этом не «прощается»: `_userIndexStampValid`
    // остаётся ложным (ниже), поэтому следующий промах сверится заново, а
    // ответом до тех пор будет «такого пользователя нет» — отказ, а не выдача
    // чужих прав.
    server::util::QuietLogError(
      "Account name index: the scan of '{}' did not finish, the index is "
      "incomplete and will be rebuilt on the next miss", server::util::LogPath(_userDataPath));
  }

  std::unordered_set<std::string> keys;
  for (const auto& filePath : listing.files)
  {
    // Тот же фильтр, что у прежнего обхода (R58-7): `.json` и только он —
    // осиротевший `Вася.json.tmp` не имеет права занять имя «Вася».
    if (filePath.extension() != ".json")
      continue;

    // ★ФАЙЛ, КОТОРЫЙ НЕ УДАЛОСЬ ПРИВЕСТИ К ПОЛИТИКЕ, В ИНДЕКС НЕ ПОПАДАЕТ
    // (правка ревью, итерация 5). Прежде перестройка индексировала ВСЕ имена
    // независимо от того, что сказал проход сужения, и тем самым объявляла
    // рабочей запись, чтение которой отказано (`ReadManagedFile` теперь даёт
    // `Failed`). Три потребителя снова разошлись бы в ответе «существует ли
    // Alice» — то самое расхождение, ради устранения которого заведён общий
    // вход в чтение. Регистрации это не открывает: `LocalAuthenticationBackend`
    // спрашивает не индекс, а сам файл, и получает `Failed`, то есть отказ, а
    // не «имя свободно».
    if (std::ranges::find(hardening.unsecured, filePath)
      != hardening.unsecured.end())
    {
      continue;
    }

    // ★КЛЮЧ БЕРЁТСЯ ИЗ ИМЕНИ ФАЙЛА, а не из JSON: именно по имени файла
    // `LocalAuthenticationBackend` открывает аккаунт, и именно оно решало исход
    // прежнего обхода. Читать здесь содержимое значило бы разобрать все
    // аккаунты на старте ради поля, которое ни на что не влияет.
    const auto stem = filePath.stem().string();
    if (stem.empty())
      continue;
    RaiseNameCeiling(_loginNameCeiling, stem.size());
    keys.insert(server::util::AsciiLowerKey(stem));
  }

  // Отпечаток считается действительным ТОЛЬКО у полной перестройки: иначе
  // «совпал» означало бы «мы уже смотрели», хотя посмотрели не всё.
  // ★И ОТКАЗ СУЖЕНИЯ ТОЖЕ ДЕЛАЕТ ОТПЕЧАТОК НЕДЕЙСТВИТЕЛЬНЫМ (правка ревью,
  // итерация 5). Прежде неудача `fchmod` записывалась в лог, а отпечаток
  // объявлялся действительным — то есть следующая сверка откладывалась до
  // изменения каталога, и файл, чей режим сузить не удалось, мог остаться
  // непопробованным сколь угодно долго. Пока хоть один файл вне политики,
  // сверка обязана повторяться.
  const std::size_t indexed = keys.size();
  {
    const std::unique_lock indexLock(_userNameIndexMutex);
    for (const auto& late : _userNamesAddedDuringScan)
      keys.insert(late);
    _userNamesAddedDuringScan.clear();
    _userIndexScanInFlight = false;
    // Публикация состоялась — сторожу больше нечего откатывать.
    scanFlagGuard.armed = false;

    _userNameKeys = std::move(keys);
    _userIndexStampValid.store(
      not stampError && not listing.incomplete
        && not hardening.incomplete && hardening.failed == 0,
      std::memory_order::relaxed);
    _userIndexDirectoryStamp = stamp;

    // ★ВРЕМЯ ОКОНЧАНИЯ, А НЕ НАЧАЛА. Именно оно ограничивает частоту: поток,
    // вошедший ДО конца этой перестройки, увидит `now - _userIndexLastScan`
    // отрицательным и перестраивать не станет.
    _userIndexLastScan = std::chrono::steady_clock::now();
  }

  server::util::QuietLogInfo(
    "Account name index: {} account names indexed", indexed);
}

void server::FileDataSource::IndexUserName(const std::string& name)
{
  if (name.empty())
    return;

  // ★НЕУДАЧА ИНДЕКСАЦИИ НЕ ИМЕЕТ ПРАВА ВЫЙТИ НАРУЖУ (правка ревью, итерация 6),
  // И ЗАЩИЩЁННЫМ ОБЯЗАН БЫТЬ ВЕСЬ ПОСЛЕЗАПИСНОЙ УЧЁТ (правка ревью, итерация 7).
  //
  // Нас зовут ПОСЛЕ успешной записи файла (`StoreUser`), поэтому бросок отсюда
  // сообщил бы вызывающему «сохранить не удалось» про запись, которая на диске
  // уже лежит, — то есть соврал бы в самую вредную сторону. Итерация 6 закрыла
  // `try` только вокруг вставок, а `AsciiLowerKey` (аллокация ключа) и взятие
  // замка стояли ВЫШЕ него: `bad_alloc` из них уходил наружу ровно тем же
  // путём, что и до правки. Теперь под защитой всё, что делается после записи.
  //
  // Индекс — это КЭШ диска: честный ответ на нехватку памяти — объявить
  // отпечаток недействительным и заставить следующий промах перечитать каталог,
  // а не отменять сохранение.
  try
  {
    RaiseNameCeiling(_loginNameCeiling, name.size());

    // ★ОТПЕЧАТОК СНИМАЕТСЯ ДО ВЗЯТИЯ ЗАМКА И ПОСЛЕ ЗАПИСИ ФАЙЛА. `StoreUser`
    // пишет файл, потом зовёт нас, поэтому этот `last_write_time` уже включает
    // нашу собственную запись. Чужой файл, появившийся ПОСЛЕ снимка, оставит
    // отпечаток разошедшимся — то есть ошибка идёт в безопасную сторону.
    std::error_code stampError;
    const auto stamp = std::filesystem::last_write_time(_userDataPath, stampError);

    auto key = server::util::AsciiLowerKey(name);

    const std::unique_lock indexLock(_userNameIndexMutex);

    // ★ЕСЛИ ПРЯМО СЕЙЧАС ИДЁТ ОБХОД — ИМЯ ЗАПОМИНАЕТСЯ ОТДЕЛЬНО (правка ревью,
    // итерация 5). Обход теперь работает СНАРУЖИ замка индекса и публикует
    // готовый набор; набор снят ДО этой регистрации, поэтому без этой строки
    // публикация затёрла бы только что зарегистрированного игрока и он остался
    // бы ненаходимым до следующей сверки.
    if (_userIndexScanInFlight)
      _userNamesAddedDuringScan.insert(key);
    _userNameKeys.insert(std::move(key));

    // ★СВОЯ ЗАПИСЬ НЕ ДЕЛАЕТ ИНДЕКС УСТАРЕВШИМ (правка ревью, итерация 3). Ключ
    // уже внесён строкой выше — индекс СОГЛАСОВАН с диском, — но mtime каталога
    // от нашей же записи изменился, и без этой строки следующий промах читал бы
    // «каталог изменился» и заказывал полный обход. Именно это позволяло
    // staff-клиенту размножать обходы: сохранить запись, спросить отсутствующее
    // имя, повторить. Принимаем новый отпечаток ТОЛЬКО когда индекс полон
    // (`_userIndexStampValid`) — у оборванной перестройки принимать нечего.
    //
    // Чужой файл, успевший появиться между записью и снимком, будет замаскирован
    // до принудительной сверки раз в `kUserIndexStaleAfter` — та существует
    // ровно для этого класса (см. `NeedsUserIndexReconcile`).
    if (_userIndexStampValid.load(std::memory_order::relaxed) && not stampError)
      _userIndexDirectoryStamp = stamp;
  }
  catch (const std::exception& x)
  {
    // ★ФЛАГ АТОМАРНЫЙ ИМЕННО РАДИ ЭТОЙ СТРОКИ (правка ревью, итерация 7): к
    // моменту перехвата замок индекса УЖЕ отпущен раскруткой стека, а объявить
    // отпечаток недействительным обязано быть возможно и тогда, когда взять
    // замок снова нечем.
    _userIndexStampValid.store(false, std::memory_order::relaxed);
    server::util::QuietLogError(
      "Account name index: '{}' could not be indexed ({}); the index is marked "
      "stale and will be rebuilt from disk on the next miss", name, x.what());
    return;
  }
}

void server::FileDataSource::CreateInfraction(data::Infraction& infraction)
{
  infraction.uid = NextUid(_infractionSequentialUid, "infraction");
  SaveMetadata();
}

void server::FileDataSource::RetrieveInfraction(data::Uid uid, data::Infraction& infraction)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
   _infractionDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Infraction file", server::util::FileSensitivity::Public);
  infraction.uid = json.value("uid", data::Uid{});
  infraction.description = json.value("description", std::string{});
  infraction.punishment = json.value("punishment", data::Infraction::Punishment{});
  infraction.duration = std::chrono::seconds(
    json.value("duration", int64_t{}));
  infraction.createdAt = data::Clock::time_point(std::chrono::seconds(
    json.value("createdAt", int64_t{})));
}

void server::FileDataSource::StoreInfraction(data::Uid uid, const data::Infraction& infraction)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _infractionDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = infraction.uid();
  json["description"] = infraction.description();
  json["punishment"] = infraction.punishment();
  json["duration"] = infraction.duration().count();
  json["createdAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    infraction.createdAt().time_since_epoch()).count();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Infraction file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteInfraction(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _infractionDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Infraction file");
}

void server::FileDataSource::CreateCharacter(data::Character& character)
{
  character.uid = NextUid(_characterSequentialUid, "character");
  SaveMetadata();
}

void server::FileDataSource::RetrieveCharacter(data::Uid uid, data::Character& character)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _characterDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Character file", server::util::FileSensitivity::Public);

  character.uid = json.value("uid", data::Uid{});
  character.name = json.value("name", std::string{});

  character.introduction = json.value("introduction", std::string{});

  character.level = json.value("level", uint32_t{});
  character.experience = json.value("experience", uint32_t{});
  character.carrots = json.value("carrots", int32_t{});
  character.cash = json.value("cash", uint32_t{});

  character.role = static_cast<data::Character::Role>(
    json.value("role", uint32_t{}));

  if (character.role() == data::Character::Role::User)
  {
    character.roleRank = data::Character::RoleRank::None;
  }
  else
  {
    character.roleRank = static_cast<data::Character::RoleRank>(
      json.value("staffRank", static_cast<uint32_t>(data::Character::RoleRank::None)));
  }

  const auto& parts = json.value("parts", nlohmann::json::object());
  character.parts = data::Character::Parts{
    .modelId = parts.value("modelId", data::Uid{}),
    .mouthId = parts.value("mouthId", data::Uid{}),
    .faceId = parts.value("faceId", data::Uid{})};

  const auto& appearance = json.value("appearance", nlohmann::json::object());
  character.appearance = data::Character::Appearance{
    .voiceId = appearance.value("voiceId", uint32_t{}),
    .headSize = appearance.value("headSize", uint32_t{}),
    .height = appearance.value("height", uint32_t{}),
    .thighVolume = appearance.value("thighVolume", uint32_t{}),
    .legVolume = appearance.value("legVolume", uint32_t{}),
    .emblemId = appearance.value("emblemId", uint32_t{})};

  character.guildUid = json.value("guildUid", data::Uid{});

  const auto& contacts = json.value("contacts", nlohmann::json::object());
  character.contacts.pending = contacts.value("pending", std::set<data::Uid>{});

  for (const auto& groupJson : contacts.value("groups", nlohmann::json::array()))
  {
    data::Character::Contacts::Group group{
      .uid = groupJson.value("uid", data::Uid{}),
      .name = groupJson.value("name", std::string{}),
      .members = groupJson.value("members", std::set<data::Uid>{}),
      .createdAt = data::Clock::time_point(std::chrono::seconds(
          groupJson.value("createdAt", int64_t{})))
    };

    character.contacts.groups().try_emplace(group.uid, group);
  }

  character.gifts = json.value("gifts", std::vector<data::Uid>{});
  character.purchases = json.value("purchases", std::vector<data::Uid>{});

  character.inventory = json.value("inventory", std::vector<data::Uid>{});
  character.characterEquipment = json.value("characterEquipment", std::vector<data::Uid>{});
  // todo: rename after larger refactor
  character.expiredEquipment = json.value("horseEquipment", std::vector<data::Uid>{});

  character.horses = json.value("horses", std::vector<data::Uid>{});
  character.horseSlotCount = json.value("horseSlotCount", uint8_t{});

  character.breedingWishlist = json.value("breedingWishlist", std::set<data::Uid>{});

  character.pets = json.value("pets", std::vector<data::Uid>{});
  character.mountUid = json.value("mountUid", data::Uid{});
  character.petUid = json.value("petUid", data::Uid{});

  character.eggs = json.value("eggs", std::vector<data::Uid>{});

  character.housing = json.value("housing", std::vector<data::Uid>{});

  character.isRanchLocked = json.value("isRanchLocked", bool{});

  // LOA-fix (R45-3, #58/R2): чтение достижений.
  //
  // ★ПЕРВОЕ: персонаж, созданный ДО этого раунда, обязан грузиться как раньше —
  // все три блока под `contains`, отсутствие ключей = пустые поля. Миграция
  // данных не нужна, и это проверено control-армом на СТАРОМ прод-профиле, а не
  // принято на веру (урок арки «Полёт»).
  //
  // ★ВТОРОЕ (находка ревью R45): `contains` говорит лишь о НАЛИЧИИ ключа, а не о
  // его пригодности. Файл персонажа пишет асинхронный тик, и он же правится
  // руками при разборах — то есть встретить null, строку вместо числа, массив
  // не той длины или оборванную запись вполне реально. Любая такая кривизна НЕ
  // должна делать персонажа незагружаемым: проверяем тип, длину и диапазон, а
  // при несоответствии оставляем поле пустым и пишем предупреждение. Потерять
  // пустые достижения безопаснее, чем потерять персонажа целиком.
  {
    //! Верхняя граница на число записей: массив приходит из файла, и раздутый
    //! файл не должен превращаться в неограниченное выделение памяти.
    constexpr std::size_t kMaxAchievementEntries = 4096;
    //! Книг всего девять (0..8).
    constexpr uint64_t kMaxBookId = 8;
    //! Секунды epoch, за которыми момент взятия тира считаем мусором:
    //! 4102444800 = 2100-01-01 UTC. Отрицательные — тоже мусор.
    constexpr int64_t kMaxTierSeconds = 4102444800;

    // Беззнаковое число в заданных границах; всё прочее (null, строка, дробь,
    // отрицательное, слишком большое) отвергается.
    const auto readBounded = [](const nlohmann::json& value,
                                const uint64_t limit,
                                uint64_t& out) -> bool
    {
      if (not value.is_number_unsigned())
        return false;
      const auto number = value.get<uint64_t>();
      if (number > limit)
        return false;
      out = number;
      return true;
    };

    if (json.contains("keyAchievements"))
    {
      const auto& value = json["keyAchievements"];
      std::array<uint16_t, 3> slots{};
      bool valid = value.is_array() and value.size() == slots.size();
      for (std::size_t index = 0; valid and index < slots.size(); ++index)
      {
        uint64_t number = 0;
        if (readBounded(value[index],
              std::numeric_limits<uint16_t>::max(), number))
          slots[index] = static_cast<uint16_t>(number);
        else
          valid = false;
      }

      if (valid)
        character.keyAchievements = slots;
      else
        server::util::QuietLogWarn(
          "Character '{}' has a malformed 'keyAchievements' field, ignoring it",
          uid);
    }

    if (json.contains("achievements") and json["achievements"].is_array())
    {
      std::vector<data::Character::AchievementEntry> entries;
      for (const auto& entry : json["achievements"])
      {
        if (entries.size() >= kMaxAchievementEntries)
        {
          server::util::QuietLogWarn(
            "Character '{}' has more than {} achievements, ignoring the rest",
            uid,
            kMaxAchievementEntries);
          break;
        }

        uint64_t tid = 0;
        if (not entry.is_object()
          or not entry.contains("tid")
          or not readBounded(entry["tid"],
                std::numeric_limits<uint16_t>::max(), tid)
          or tid == 0)
        {
          // Запись без пригодного tid не описывает ничего — пропускаем её, а не
          // роняем загрузку персонажа.
          continue;
        }

        // Один tid не может иметь два прогресса; побеждает первая запись.
        const auto duplicate = std::ranges::find(
          entries, static_cast<uint16_t>(tid),
          &data::Character::AchievementEntry::tid);
        if (duplicate != entries.end())
          continue;

        data::Character::AchievementEntry achievement{
          .tid = static_cast<uint16_t>(tid)};

        uint64_t progress = 0;
        if (entry.contains("progress")
          and readBounded(entry["progress"],
                std::numeric_limits<uint32_t>::max(), progress))
        {
          achievement.progress = static_cast<uint32_t>(progress);
        }

        if (entry.contains("tierEarnedAt"))
        {
          const auto& tiers = entry["tierEarnedAt"];
          if (tiers.is_array()
            and tiers.size() == achievement.tierEarnedAt.size())
          {
            for (std::size_t tier = 0; tier < tiers.size(); ++tier)
            {
              uint64_t seconds = 0;
              if (not readBounded(tiers[tier], kMaxTierSeconds, seconds)
                or seconds == 0)
              {
                // Ноль = тир не взят; мусор трактуем так же — это ровно то
                // значение, которое поле имеет по умолчанию.
                continue;
              }
              achievement.tierEarnedAt[tier] = data::Clock::time_point{
                std::chrono::seconds{static_cast<int64_t>(seconds)}};
            }
          }
          else
          {
            server::util::QuietLogWarn(
              "Character '{}' has a malformed 'tierEarnedAt' on achievement "
              "{}, treating every tier as not earned",
              uid,
              tid);
          }
        }

        entries.push_back(achievement);
      }
      character.achievements = std::move(entries);
    }

    if (json.contains("achievementBooks") and json["achievementBooks"].is_array())
    {
      std::vector<data::Character::AchievementBookEntry> books;
      for (const auto& entry : json["achievementBooks"])
      {
        if (books.size() > kMaxBookId)
          break;

        uint64_t bookId = 0;
        if (not entry.is_object()
          or not entry.contains("bookId")
          or not readBounded(entry["bookId"], kMaxBookId, bookId))
        {
          continue;
        }

        const auto duplicate = std::ranges::find(
          books, static_cast<uint8_t>(bookId),
          &data::Character::AchievementBookEntry::bookId);
        if (duplicate != books.end())
          continue;

        data::Character::AchievementBookEntry book{
          .bookId = static_cast<uint8_t>(bookId)};

        if (entry.contains("tierRewardClaimed"))
        {
          const auto& claimed = entry["tierRewardClaimed"];
          if (claimed.is_array()
            and claimed.size() == book.tierRewardClaimed.size())
          {
            for (std::size_t tier = 0; tier < claimed.size(); ++tier)
            {
              uint64_t number = 0;
              if (readBounded(claimed[tier],
                    std::numeric_limits<uint32_t>::max(), number))
                book.tierRewardClaimed[tier] = static_cast<uint32_t>(number);
            }
          }
          else
          {
            server::util::QuietLogWarn(
              "Character '{}' has a malformed 'tierRewardClaimed' on book {}, "
              "treating every tier reward as unclaimed",
              uid,
              bookId);
          }
        }

        books.push_back(book);
      }
      character.achievementBooks = std::move(books);
    }

    // LOA (R75, #14): пер-курсовые рекорды. Читаем ЗАЩИЩЁННО, как достижения:
    // мусорная запись пропускается, а не роняет загрузку персонажа; дубли по
    // courseId побеждает первая запись (двух рекордов на одну трассу не бывает);
    // список режется тем же капом, что несёт провод.
    // ★СТОИМ ВНУТРИ ЭТОГО БЛОКА НАМЕРЕННО: лямбда readBounded объявлена здесь и
    // живёт до закрывающей скобки блока. Снаружи её нет.
    if (json.contains("courseRecords") and json["courseRecords"].is_array())
    {
      std::vector<data::Character::CourseRecord> records;
      // ★reserve на весь кап: рост вектора на ГОРЯЧЕМ пути RaceInstance::Stop()
      // идёт внутри noexcept-тела пояса, и чем реже там случается реаллокация,
      // тем уже окно теоретического bad_alloc (там он выловлен, но лучше в него
      // не входить).
      records.reserve(data::MaxCourseRecords);
      for (const auto& entry : json["courseRecords"])
      {
        if (records.size() >= data::MaxCourseRecords)
        {
          server::util::QuietLogWarn(
            "Character '{}' has more than {} course records, ignoring the rest",
            uid, data::MaxCourseRecords);
          break;
        }
        uint64_t courseId = 0;
        if (not entry.is_object()
          or not entry.contains("courseId")
          or not readBounded(entry["courseId"],
                std::numeric_limits<uint16_t>::max(), courseId)
          or courseId == 0)
          continue;
        if (std::ranges::find(records, static_cast<uint16_t>(courseId),
              &data::Character::CourseRecord::courseId) != records.end())
          continue;

        data::Character::CourseRecord record{
          .courseId = static_cast<uint16_t>(courseId)};
        uint64_t value = 0;
        if (entry.contains("recordTime")
          and readBounded(entry["recordTime"],
                std::numeric_limits<uint32_t>::max(), value))
          record.recordTime = static_cast<uint32_t>(value);
        value = 0;
        if (entry.contains("timesRaced")
          and readBounded(entry["timesRaced"],
                std::numeric_limits<uint32_t>::max(), value))
          record.timesRaced = static_cast<uint32_t>(value);

        records.push_back(record);
      }
      character.courseRecords = std::move(records);
    }
    character.totalSpeedGames = json.value("totalSpeedGames", uint32_t{});
    character.totalMagicGames = json.value("totalMagicGames", uint32_t{});
  }

  character.settingsUid = json.value("settingsUid", data::Uid{});

  const auto readSkills = [](data::Character::Skills::Sets& sets, const nlohmann::json& json)
  {
    const auto readSkillSet = [](data::Character::Skills::Sets::Set& set, const nlohmann::json& json)
    {
      set.slot1 = json.value("slot1", uint32_t{});
      set.slot2 = json.value("slot2", uint32_t{});
    };

    readSkillSet(sets.set1, json.value("set1", nlohmann::json::object()));
    readSkillSet(sets.set2, json.value("set2", nlohmann::json::object()));
    sets.activeSetId = json.value("activeSetId", uint32_t{});
  };

  const auto& skills = json.value("skills", nlohmann::json::object());
  readSkills(character.skills.speed(), skills.value("speed", nlohmann::json::object()));
  readSkills(character.skills.magic(), skills.value("magic", nlohmann::json::object()));

  character.dailyQuestGroupUid = json.value("dailyQuestGroupUid", data::InvalidUid);
  const auto& mailbox = json.value("mailbox", nlohmann::json::object());
  character.mailbox.hasNewMail = mailbox.value("hasNewMail", bool{});
  character.mailbox.inbox = mailbox.value("inbox", std::vector<data::Uid>{});
  character.mailbox.sent = mailbox.value("sent", std::vector<data::Uid>{});

  character.quests = json.value("quests", std::vector<data::Uid>{});

  // LOA (batch2): care-skill state. Zero-touch migration — old files lack the
  // "careSkills" object and load as all-zero / empty.
  const auto& careSkills = json.value("careSkills", nlohmann::json::object());
  character.careSkills.carePoints = careSkills.value("carePoints", uint32_t{});
  character.careSkills.careClassLevel = careSkills.value("careClassLevel", uint8_t{});
  character.careSkills.careProgress = careSkills.value("careProgress", uint32_t{});
  std::vector<data::Character::CareSkills::LearnedSkill> learnedRanks;
  for (const auto& learnedJson : careSkills.value("learnedRanks", nlohmann::json::array()))
  {
    data::Character::CareSkills::LearnedSkill learned{};
    learned.id = learnedJson.value("id", uint8_t{});
    learned.rank = learnedJson.value("rank", uint8_t{});
    learnedRanks.push_back(learned);
  }
  character.careSkills.learnedRanks = learnedRanks;

  // LOA (R65, backlog #175): карантин снятых ссылок. Тот же нулевой перенос,
  // что у careSkills — старый файл ключа не имеет и читается как пустой.
  std::vector<data::Character::DamagedReference> damagedReferences;
  for (const auto& damagedJson : json.value("damagedReferences", nlohmann::json::array()))
  {
    data::Character::DamagedReference damaged{};
    damaged.uid = damagedJson.value("uid", data::InvalidUid);
    damaged.kind = damagedJson.value("kind", std::string{});
    damagedReferences.push_back(damaged);
  }
  character.damagedReferences = damagedReferences;
}

void server::FileDataSource::StoreCharacter(data::Uid uid, const data::Character& character)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _characterDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = character.uid();
  json["name"] = character.name();

  json["introduction"] = character.introduction();

  json["level"] = character.level();
  json["experience"] = character.experience();
  json["carrots"] = character.carrots();
  json["cash"] = character.cash();

  json["role"] = character.role();
  json["staffRank"] = character.roleRank();

  // Character parts
  nlohmann::json parts;
  parts["modelId"] = character.parts.modelId();
  parts["mouthId"] = character.parts.mouthId();
  parts["faceId"] = character.parts.faceId();
  json["parts"] = parts;

  // Character appearance
  nlohmann::json appearance;
  appearance["voiceId"] = character.appearance.voiceId();
  appearance["headSize"] = character.appearance.headSize();
  appearance["height"] = character.appearance.height();
  appearance["thighVolume"] = character.appearance.thighVolume();
  appearance["legVolume"] = character.appearance.legVolume();
  appearance["emblemId"] = character.appearance.emblemId();
  json["appearance"] = appearance;

  json["guildUid"] = character.guildUid();

  nlohmann::json contacts;
  contacts["pending"] = character.contacts.pending();

  nlohmann::json groups;
  for (const auto& group : character.contacts.groups() | std::views::values)
  {
    nlohmann::json groupJson;
    groupJson["uid"] = group.uid;
    groupJson["name"] = group.name;
    groupJson["members"] = group.members;
    groupJson["createdAt"] = std::chrono::ceil<std::chrono::seconds>(
      group.createdAt.time_since_epoch()).count();

    groups.emplace_back(groupJson);
  }
  contacts["groups"] = groups;

  json["contacts"] = contacts;

  json["gifts"] = character.gifts();
  json["purchases"] = character.purchases();

  json["inventory"] = character.inventory();
  json["characterEquipment"] = character.characterEquipment();
  json["horseEquipment"] = character.expiredEquipment();

  json["horses"] = character.horses();
  json["horseSlotCount"] = character.horseSlotCount();

  json["breedingWishlist"] = character.breedingWishlist();

  json["pets"] = character.pets();
  json["mountUid"] = character.mountUid();
  json["petUid"] = character.petUid();

  json["eggs"] = character.eggs();

  json["housing"] = character.housing();

  json["isRanchLocked"] = character.isRanchLocked();

  // LOA-fix (R45-4, #58/R2): запись достижений. Моменты взятия тиров кладём
  // СЕКУНДАМИ epoch — тем же представлением, что и остальные времена в наших
  // файлах, чтобы запись читалась глазами и не зависела от разрядности
  // Clock::duration.
  json["keyAchievements"] = character.keyAchievements();

  {
    auto achievementsJson = nlohmann::json::array();
    for (const auto& entry : character.achievements())
    {
      std::array<int64_t, 4> tierEarnedAt{};
      for (size_t tier = 0; tier < tierEarnedAt.size(); ++tier)
      {
        // ★Гранулярность хранения — СЕКУНДА (находка ревью R45): часы идут в
        // наносекундах, поэтому запись усечёт долю секунды, и обратное чтение
        // даст момент чуть раньше исходного. Для достижений это безразлично —
        // клиенту показывается ДАТА взятия тира, — но знать об этом надо: после
        // первого сохранения значение уже секундное и дальше round-trip точен.
        // Отрицательное (момент до 1970) записываем нулём: ноль по нашей же
        // схеме означает «тир не взят», а до-эпошного взятия не бывает.
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
          entry.tierEarnedAt[tier].time_since_epoch()).count();
        tierEarnedAt[tier] = seconds > 0 ? seconds : 0;
      }

      achievementsJson.push_back({{"tid", entry.tid},
        {"progress", entry.progress},
        {"tierEarnedAt", tierEarnedAt}});
    }
    json["achievements"] = std::move(achievementsJson);
  }

  {
    auto booksJson = nlohmann::json::array();
    for (const auto& entry : character.achievementBooks())
    {
      booksJson.push_back({{"bookId", entry.bookId},
        {"tierRewardClaimed", entry.tierRewardClaimed}});
    }
    json["achievementBooks"] = std::move(booksJson);
  }

  // LOA (R75, #14): пер-курсовые рекорды и счётчики заездов. Пишем БЕЗУСЛОВНО —
  // как achievements/careSkills: это часть каждой записи, а не карантин, который
  // у здорового персонажа обязан отсутствовать.
  {
    auto courseRecordsJson = nlohmann::json::array();
    for (const auto& entry : character.courseRecords())
    {
      courseRecordsJson.push_back({{"courseId", entry.courseId},
        {"recordTime", entry.recordTime},
        {"timesRaced", entry.timesRaced}});
    }
    json["courseRecords"] = std::move(courseRecordsJson);
  }
  json["totalSpeedGames"] = character.totalSpeedGames();
  json["totalMagicGames"] = character.totalMagicGames();

  json["settingsUid"] = character.settingsUid();

  // Construct game mode skills from skill sets
  const auto& writeSkills = [](const data::Character::Skills::Sets& sets)
  {
    const auto& writeSkillSet = [](const data::Character::Skills::Sets::Set& set)
    {
      nlohmann::json json;
      json["slot1"] = set.slot1;
      json["slot2"] = set.slot2;
      return json;
    };

    nlohmann::json json;
    json["set1"] = writeSkillSet(sets.set1);
    json["set2"] = writeSkillSet(sets.set2);
    json["activeSetId"] = sets.activeSetId;
    return json;
  };

  nlohmann::json skills;
  skills["speed"] = writeSkills(character.skills.speed());
  skills["magic"] = writeSkills(character.skills.magic());
  json["skills"] = skills;

  json["dailyQuestGroupUid"] = character.dailyQuestGroupUid();
  nlohmann::json mailbox;
  mailbox["hasNewMail"] = character.mailbox.hasNewMail();
  mailbox["inbox"] = character.mailbox.inbox();
  mailbox["sent"] = character.mailbox.sent();
  json["mailbox"] = mailbox;

  json["quests"] = character.quests();

  // LOA (batch2): persist care-skill state.
  nlohmann::json careSkills;
  careSkills["carePoints"] = character.careSkills.carePoints();
  careSkills["careClassLevel"] = character.careSkills.careClassLevel();
  careSkills["careProgress"] = character.careSkills.careProgress();
  nlohmann::json learnedRanksJson = nlohmann::json::array();
  for (const auto& learned : character.careSkills.learnedRanks())
  {
    learnedRanksJson.push_back({
      {"id", learned.id},
      {"rank", learned.rank}
    });
  }
  careSkills["learnedRanks"] = learnedRanksJson;
  json["careSkills"] = careSkills;

  // LOA (R65, backlog #175): карантин пишется ТОЛЬКО когда он непуст.
  // ★Условие здесь не ради красоты файла, а ради радиуса раунда: у здорового
  // персонажа (а это все) сохранённый файл обязан остаться ПОБАЙТОВО тем же,
  // что и до раунда. Безусловный `json["damagedReferences"] = []` изменил бы
  // каждый файл в проде ради поля, которое почти всегда пусто.
  if (not character.damagedReferences().empty())
  {
    nlohmann::json damagedReferencesJson = nlohmann::json::array();
    for (const auto& damaged : character.damagedReferences())
    {
      damagedReferencesJson.push_back({
        {"uid", damaged.uid},
        {"kind", damaged.kind}
      });
    }
    json["damagedReferences"] = damagedReferencesJson;
  }

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Character file",
    server::util::FileSensitivity::Public);

  // ★ИНДЕКС ОБНОВЛЯЕТСЯ ПОСЛЕ УСПЕШНОЙ ЗАПИСИ, а не до неё. Бросок из
  // `WriteFileAtomically` оставляет на диске СТАРОЕ имя — индекс обязан остаться
  // согласованным с диском, а не с намерением.
  IndexCharacterName(uid, character.name());
}

void server::FileDataSource::DeleteCharacter(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _characterDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Character file");
  ForgetCharacterName(uid);
}

void server::FileDataSource::RebuildCharacterNameIndex()
{
  const std::unique_lock indexLock(_characterNameIndexMutex);
  // ★ФЛАГ ПОЛНОТЫ СНИМАЕТСЯ ПЕРВЫМ ДЕЙСТВИЕМ (правка ревью, итерация 7): бросок
  // посреди обхода обязан оставить индекс объявленным НЕПОЛНЫМ.
  _characterNameIndexComplete.store(false, std::memory_order::relaxed);
  _characterNameToUid.clear();
  _characterUidToName.clear();

  //! Сколько записей мы НЕ СМОГЛИ прочитать. Ноль — и только ноль — даёт право
  //! отвечать «имя свободно» (правка ревью, итерация 7).
  std::size_t unresolved = 0;
  std::size_t duplicates = 0;

  const auto listing = server::util::ListRegularFiles(_characterDataPath);
  if (listing.incomplete)
  {
    // ★НЕПОЛНЫЙ ИНДЕКС НА СТАРТЕ ФАТАЛЕН (правка ревью, итерация 3). Прежняя
    // редакция печатала строку и продолжала — то есть сервер начинал
    // обслуживать игроков, у которых ЖИВОЙ персонаж не находится по имени, а
    // его имя при этом числится свободным: создание персонажа с тем же именем
    // прошло бы «уникальность». Строка в логе не отменяет ни того, ни другого.
    //
    // Перестройка зовётся РОВНО из `Initialize` (см. объявление в заголовке),
    // поэтому бросок здесь останавливает старт, а не рвёт живой запрос.
    server::util::QuietLogError(
      "Character name index: the scan of '{}' did not finish; refusing to start "
      "with live characters unaddressable by name and their names readable as free",
      server::util::LogPath(_characterDataPath));

    throw std::runtime_error(
      std::format(
        "Character name index scan of '{}' did not finish",
        server::util::LogPath(_characterDataPath)));
  }

  // ★ОТВЕРГНУТАЯ ССЫЛКА НАЗЫВАЕТСЯ ВСЛУХ И ЗДЕСЬ (правка ревью, итерация 4).
  // Она не индексируется — читать её `ReadManagedFile` отказывается, — но её
  // ИМЯ уже учтено полом счётчика uid (см. `HighestUidInDirectory`), поэтому
  // uid под ней не переиспользуется. Молчание же означало бы отчёт о полном
  // индексе над каталогом, часть которого мы отказались смотреть.
  if (not listing.refusedSymlinks.empty())
  {
    // ★«НЕ СТАЛИ СМОТРЕТЬ» РАВНО «НЕ ВИДЕЛИ» (правка ревью, итерация 7): имя под
    // отвергнутой ссылкой неизвестно, а неизвестное имя не имеет права читаться
    // как свободное. Прежде это была только строка в логе.
    unresolved += listing.refusedSymlinks.size();
    server::util::QuietLogWarn(
      "Character name index: {} entry(ies) in '{}' are symbolic links and were "
      "refused; they are not indexed and their uids stay reserved",
      listing.refusedSymlinks.size(), server::util::LogPath(_characterDataPath));
  }

  for (const auto& filePath : listing.files)
  {
    // Тот же фильтр, что у прежнего обхода (R58-8): `.json` и только он.
    if (filePath.extension() != ".json")
      continue;

    // ★ЛИЧНОСТЬ ЗАПИСИ — ИМЯ ФАЙЛА, А НЕ `json["uid"]` (правка ревью, итерация
    // 7). `StoreCharacter` и `DeleteCharacter` адресуют файл ИМЕНЕМ, поэтому
    // индекс, взявший uid из содержимого, вёл бы учёт про другую запись:
    // `7.json` с полем `uid: 8` делал `DeleteCharacter(7)` неспособным
    // освободить имя, а два файла с одним полем `uid` освобождали живое имя.
    const auto parsedUid = ParseRecordUid(filePath.stem().string());
    if (not parsedUid)
    {
      ++unresolved;
      server::util::QuietLogWarn(
        "Character file '{}' is not named after a record identifier; the index "
        "refuses to guess whose name it carries",
        server::util::LogPath(filePath));
      continue;
    }
    const data::Uid existingUid = *parsedUid;

    const auto read = server::util::ReadManagedFile(
      filePath, server::util::FileSensitivity::Public);
    if (read.status != server::util::ManagedReadStatus::Ok)
    { ++unresolved; continue; }

    std::string existingName;
    try
    {
      const auto json = nlohmann::json::parse(read.content);
      existingName = json.value("name", std::string{});
    }
    catch (const std::exception& x)
    {
      // ★ПЕРЕХВАТ СОХРАНЁН ПО СМЫСЛУ (R58-8): один битый файл не имеет права
      // сломать поиск по имени и создание персонажа У ВСЕХ. Текст строки НОВЫЙ
      // намеренно — старая строка «skipped while looking up a character by
      // name» ОБЯЗАНА исчезнуть из бинаря, это маркер лесенки.
      server::util::QuietLogWarn(
        "Character file '{}' is unreadable ({}) and was skipped while building "
        "the character name index", server::util::LogPath(filePath), x.what());
      ++unresolved;
      continue;
    }

    if (existingName.empty())
    { ++unresolved; continue; }

    auto key = server::util::AsciiLowerKey(existingName);

    // ★ОДИН uid ЖИВЁТ РОВНО В ОДНОМ СПИСКЕ. Два файла вправе объявить один и тот
    // же `uid` в JSON (имя файла и поле внутри независимы), и без этой ветки
    // второй файл оставил бы первый uid болтаться в чужом списке навсегда.
    const auto alreadyIndexed = _characterUidToName.find(existingUid);
    if (alreadyIndexed != _characterUidToName.end())
    {
      if (alreadyIndexed->second == key)
        continue;
      DetachNameKey(_characterNameToUid, alreadyIndexed->second, existingUid);
    }

    // ★ВСЕ СТОЛКНУВШИЕСЯ uid ОСТАЮТСЯ В ИНДЕКСЕ, разрешает имя МЕНЬШИЙ. Прежний
    // обход возвращал того, кто раньше попался `directory_iterator`, — порядок
    // не был определён стандартом; прежняя редакция индекса выбирала меньший, но
    // ПРОИГРАВШИХ ВЫБРАСЫВАЛА, и снятие победителя (переименование, удаление)
    // делало имя неразрешимым до перезапуска. Найдено ревью (итерация 1).
    const std::size_t collisions =
      AttachNameKey(_characterNameToUid, key, existingUid);
    RaiseNameCeiling(_characterNameCeiling, existingName.size());
    if (collisions > 1)
    {
      ++duplicates;
      const auto& bucket = _characterNameToUid.find(key)->second;
      server::util::QuietLogWarn(
        "Duplicate character name '{}' in the data directory: uid {} resolves "
        "the name, {} further uid(s) kept shadowed",
        existingName, bucket.front(), bucket.size() - 1);
    }
    _characterUidToName[existingUid] = std::move(key);
  }

  _characterNameIndexComplete.store(
    unresolved == 0, std::memory_order::relaxed);
  _characterIndexLastRetry.store(
    std::chrono::steady_clock::now(), std::memory_order::relaxed);

  server::util::QuietLogInfo(
    "Character name index: {} names indexed, {} files unresolved, {} duplicates",
    _characterNameToUid.size(), unresolved, duplicates);

  if (unresolved != 0)
  {
    server::util::QuietLogError(
      "Character name index: {} character file(s) in '{}' could not be read; "
      "until they can be, EVERY character name reads as taken rather than free",
      unresolved, server::util::LogPath(_characterDataPath));
  }
}

void server::FileDataSource::IndexCharacterName(
  const data::Uid uid, const std::string& name)
{
  // ★ПУСТОЕ ИМЯ СНИМАЕТ ЗАПИСЬ, А НЕ ЗАВОДИТ КЛЮЧ "". `RebuildCharacterNameIndex`
  // пропускает записи с пустым именем, и без этой строки персонаж, сохранённый
  // с пустым именем, занимал бы ключ "" в рантайме и исчезал бы после рестарта —
  // ровно тот класс «состояние живёт дольше рестарта и расходится с диском»,
  // который раунд и убирает.
  if (name.empty())
  {
    ForgetCharacterName(uid);
    return;
  }

  // ★ОТКАЗ ПОДДЕРЖАНИЯ — ЭТО «ИНДЕКС НЕПОЛОН», А НЕ ПОЛУПЕРЕПИСАННЫЙ ИНДЕКС
  // (правка ревью, итерация 7; та же правка, что у гильдий, — один класс, одно
  // правило). Снятие старого ключа стоит ПЕРЕД выделением нового ведра, и
  // бросок между ними оставлял живое имя вне индекса, то есть читаемым как
  // свободное.
  try
  {
    auto key = server::util::AsciiLowerKey(name);
    RaiseNameCeiling(_characterNameCeiling, name.size());
    const std::unique_lock indexLock(_characterNameIndexMutex);
    const auto previous = _characterUidToName.find(uid);
    if (previous != _characterUidToName.end())
    {
      if (previous->second == key)
        return;                                 // имя не менялось
      // ★СНИМАЕТСЯ РОВНО НАШ uid, а список старого имени остаётся жить. Если под
      // тем же именем стоял ещё кто-то (столкновение регистров), он МОЛЧА
      // становится тем, кто это имя разрешает, — вместо того чтобы имя исчезло.
      DetachNameKey(_characterNameToUid, previous->second, uid);
    }
    AttachNameKey(_characterNameToUid, key, uid);
    _characterUidToName[uid] = std::move(key);
  }
  catch (const std::exception& x)
  {
    MarkCharacterNameIndexBroken("a character name could not be indexed", x.what());
  }
}

void server::FileDataSource::ForgetCharacterName(const data::Uid uid)
{
  const std::unique_lock indexLock(_characterNameIndexMutex);
  const auto previous = _characterUidToName.find(uid);
  if (previous == _characterUidToName.end())
    return;
  // ★УДАЛЕНИЕ ПОБЕДИТЕЛЯ ПОДНИМАЕТ СЛЕДУЮЩЕГО, а не гасит имя: список хранит
  // всех, кто это имя носит, и `DetachNameKey` убирает ключ, только когда после
  // снятия не осталось никого.
  DetachNameKey(_characterNameToUid, previous->second, uid);
  _characterUidToName.erase(previous);
}

server::FileDataSource::NameIndexAnswer server::FileDataSource::ReadNameIndexAnswer(
  std::shared_mutex& mutex,
  const std::unordered_map<std::string, std::vector<data::Uid>>& index,
  const std::atomic_bool& complete,
  const std::string& key)
{
  // ★ОДИН ЗАМОК НА ОБА ФАКТА. Полнота — атомарная переменная и читаться могла бы
  // где угодно; она читается ЗДЕСЬ намеренно, потому что важна не полнота сама
  // по себе, а полнота ТОГО СОДЕРЖИМОГО, которое мы только что видели. Читать
  // её вторым действием значило бы отвечать по содержимому одного поколения
  // индекса и по полноте другого.
  const std::shared_lock indexLock(mutex);
  NameIndexAnswer answer;
  answer.complete = complete.load(std::memory_order::relaxed);
  const auto found = index.find(key);
  if (found != index.end() && not found->second.empty())
  {
    // Список отсортирован по возрастанию — имя разрешает МЕНЬШИЙ uid, то есть
    // старшая запись, и это правило одно и то же на старте и в рантайме.
    answer.uid = found->second.front();
  }
  return answer;
}

server::data::Uid server::FileDataSource::RetrieveCharacterUidByName(const std::string_view& name)
{
  // ★СТРУКТУРНЫЙ ГЕЙТ ДО ВСЕГО — до аллокации ключа, до хеша, до диска. Провод
  // отдаёт до ~8190 байт (Stream.cpp при потолке CommandServer.cpp
  // `MaxCommandDataSize`), а хранимое имя не длиннее 36 байт UTF-8 (вывод — в
  // NameGuard.hpp).
  //
  // ★ПОТОЛОК БЕРЁТСЯ ИЗ ИНДЕКСА, А НЕ ИЗ КОНСТАНТЫ (правка ревью, итерация 1).
  // Индекс принимает ЛЮБОЕ имя, лежащее на диске; константа 64 отбивала запрос
  // ДО индекса. Персонаж, сохранённый до появления `IsNameValid` с именем в 65
  // байт, был бы проиндексирован и при этом вечно неадресуем — притом что
  // прежний точный поиск его находил. Потолок = максимум(64, самое длинное
  // проиндексированное имя): граница остаётся конечной и не режет живых.
  if (not server::util::IsStorableNameShaped(
    name, _characterNameCeiling.load(std::memory_order::relaxed)))
  {
    _rejectedNameLookups.fetch_add(1, std::memory_order::relaxed);
    return data::InvalidUid;
  }

  // ★ИНДЕКС ВМЕСТО ОБХОДА. Прежняя редакция открывала и РАЗБИРАЛА КАЖДЫЙ файл
  // персонажа на КАЖДЫЙ вызов, а зовут её шесть аутентифицированных хендлеров:
  // подарок (RanchDirector), вызов персонажа, приглашение в гонку
  // (RaceNetworkHandler), добавление друга и письмо (MessengerDirector). Один
  // пакет = O(все персонажи) обращений к диску.
  const auto key = server::util::AsciiLowerKey(name);
  return ReadNameIndexAnswer(
    _characterNameIndexMutex, _characterNameToUid,
    _characterNameIndexComplete, key).uid;
}

bool server::FileDataSource::IsCharacterNameUnique(const std::string_view& name)
{
  // ★ТОТ ЖЕ ОТВЕТ, ЧТО У ГИЛЬДИЙ, И ПО ТОЙ ЖЕ ПРИЧИНЕ (правка ревью, итерация
  // 7). Структурный гейт у персонажей есть, но он стоит в ПОИСКЕ
  // (`RetrieveCharacterUidByName`), а поиск отвечает «не нашёл» — и здесь это
  // превращалось в «имя свободно». Персонаж, созданный с именем, которое
  // физически не может лежать на диске (управляющий байт, разделитель пути,
  // длиннее потолка индекса), был бы НАВСЕГДА НЕАДРЕСУЕМ: подарок, приглашение
  // в заезд, друг и письмо ходят через тот же поиск, который это имя отбивает.
  //
  // Создание обязано отказать, а не выдать «свободно», — ровно как у гильдий.
  // Направление отличается от поиска намеренно: поиск отвечает «такого нет»,
  // создание — «так назвать нельзя». Сегодня оба вызывающих
  // (`LobbyNetworkHandler`, `RanchDirector`) зовут `locale::IsNameValid` с
  // потолком 18 байт до этого места, то есть свойство держалось их
  // вежливостью; правило переезжает в хранилище, где ему и место.
  if (not server::util::IsStorableNameShaped(
    name, _characterNameCeiling.load(std::memory_order::relaxed)))
  {
    _rejectedNameLookups.fetch_add(1, std::memory_order::relaxed);
    return false;
  }

  // ★ОТВЕТ СНИМАЕТСЯ ОДНИМ СНИМКОМ (правка ревью, итерация 9). Прежде здесь
  // стояли ДВА чтения: `RetrieveCharacterUidByName` под своим общим замком и
  // отдельное чтение флага полноты после него. Между ними умещалась чужая
  // перестройка целиком, и промах по СТАРОМУ неполному индексу читался вместе с
  // НОВОЙ полнотой как «имя свободно» — притом что имя в новом индексе уже
  // лежало.
  const auto key = server::util::AsciiLowerKey(name);
  auto answer = ReadNameIndexAnswer(
    _characterNameIndexMutex, _characterNameToUid,
    _characterNameIndexComplete, key);

  // Имя, которое индекс РАЗРЕШАЕТ, занято — тут спорить не о чем.
  if (answer.uid != data::InvalidUid)
    return false;
  if (answer.complete)
    return true;

  // ★ПРОМАХ ПО НЕПОЛНОМУ ИНДЕКСУ — НЕ «СВОБОДНО» (правка ревью, итерация 7).
  // Сначала одна попытка починиться, и только потом ответ; ПОСЛЕ починки снимок
  // берётся ЗАНОВО и целиком — иначе мы отвечали бы по полноте нового индекса и
  // содержимому старого, то есть ровно тем расхождением, ради которого снимок и
  // заведён.
  ReconcileCharacterNameIndexIfBroken();
  answer = ReadNameIndexAnswer(
    _characterNameIndexMutex, _characterNameToUid,
    _characterNameIndexComplete, key);
  if (answer.uid != data::InvalidUid)
    return false;
  return answer.complete;
}

void server::FileDataSource::CreateHorse(data::Horse& horse)
{
  horse.uid = NextUid(_equipmentSequentialUid, "equipment");
  SaveMetadata();
}

void server::FileDataSource::RetrieveHorse(data::Uid uid, data::Horse& horse)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _horseDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Horse file", server::util::FileSensitivity::Public);
  horse.uid = json.value("uid", data::Uid{});
  horse.tid = json.value("tid", data::Tid{});
  horse.name = json.value("name", std::string{});

  const auto& parts = json.value("parts", nlohmann::json::object());
  horse.parts = data::Horse::Parts{
    .skinTid = parts.value("skinId", uint32_t{}),
    .faceTid = parts.value("faceId", uint32_t{}),
    .maneTid = parts.value("maneId", uint32_t{}),
    .tailTid = parts.value("tailId", uint32_t{})};

  const auto& appearance = json.value("appearance", nlohmann::json::object());
  horse.appearance = data::Horse::Appearance{
    .scale = appearance.value("scale", uint32_t{}),
    .legLength = appearance.value("legLength", uint32_t{}),
    .legVolume = appearance.value("legVolume", uint32_t{}),
    .bodyLength = appearance.value("bodyLength", uint32_t{}),
    .bodyVolume = appearance.value("bodyVolume", uint32_t{})};

  const auto& stats = json.value("stats", nlohmann::json::object());
  horse.stats = data::Horse::Stats{
    .agility = stats.value("agility", uint32_t{}),
    .courage = stats.value("courage", uint32_t{}),
    .rush = stats.value("rush", uint32_t{}),
    .endurance = stats.value("endurance", uint32_t{}),
    .ambition = stats.value("ambition", uint32_t{})};

  const auto& mastery = json.value("mastery", nlohmann::json::object());
  horse.mastery = data::Horse::Mastery{
    .spurMagicCount = mastery.value("spurMagicCount", uint32_t{}),
    .jumpCount = mastery.value("jumpCount", uint32_t{}),
    .slidingTime = mastery.value("slidingTime", uint32_t{}),
    .glidingDistance = mastery.value("glidingDistance", uint32_t{})};

  const auto& mountCondition = json.value("mountCondition", nlohmann::json::object());
  horse.mountCondition = data::Horse::MountCondition{
    .stamina = mountCondition.value("stamina", uint32_t{}),
    .charm = mountCondition.value("charm", uint32_t{}),
    .friendliness = mountCondition.value("friendliness", uint32_t{}),
    .injury = mountCondition.value("injury", uint32_t{}),
    .plenitude = mountCondition.value("plenitude", uint32_t{}),
    .bodyDirtiness = mountCondition.value("bodyDirtiness", uint32_t{}),
    .maneDirtiness = mountCondition.value("maneDirtiness", uint32_t{}),
    .tailDirtiness = mountCondition.value("tailDirtiness", uint32_t{}),
    .bodyPolish = mountCondition.value("bodyPolish", uint32_t{}),
    .manePolish = mountCondition.value("manePolish", uint32_t{}),
    .tailPolish = mountCondition.value("tailPolish", uint32_t{}),
    .attachment = mountCondition.value("attachment", uint32_t{}),
    .boredom = mountCondition.value("boredom", uint32_t{}),
    .stopAmendsPoint = mountCondition.value("stopAmendsPoint", uint32_t{})};

  horse.rating = json.value("rating", uint32_t{});
  horse.clazz = json.value("clazz", uint32_t{});
  horse.clazzProgress = json.value("clazzProgress", uint32_t{});
  horse.grade = json.value("grade", uint32_t{});
  horse.growthPoints = json.value("growthPoints", uint32_t{});

  const auto& potential = json.value("potential", nlohmann::json::object());
  horse.potential = data::Horse::Potential{
    .type = potential.value("type", uint32_t{}),
    .level = potential.value("level", uint32_t{}),
    .value = potential.value("value", uint32_t{})
  };

  horse.luckState = json.value("luckState", uint32_t{});
  horse.fatigue = json.value("fatigue", uint32_t{});
  horse.emblemUid = json.value("emblem", uint32_t{});
  horse.tendency = json.value("tendency", uint32_t{});
  horse.spirit = json.value("spirit", uint32_t{});

  horse.type = json.value("type", data::Horse::Type{});
  horse.breedingCount = json.value("breedingCount", uint32_t{});
  horse.breedingCombo = json.value("breedingCombo", uint32_t{});
  horse.lineage = json.value("lineage", uint32_t{1});

  const auto& ancestors = json.value("ancestors", nlohmann::json::object());
  horse.ancestors = data::Horse::Ancestors{
    .father = ancestors.value("father", data::Uid{data::InvalidUid}),
    .mother = ancestors.value("mother", data::Uid{data::InvalidUid})};

  horse.dateOfBirth = data::Clock::time_point(std::chrono::seconds(
    json.value("dateOfBirth", uint64_t{})));

  const auto& mountInfo = json.value("mountInfo", nlohmann::json::object());
  horse.mountInfo = data::Horse::MountInfo{
    .boostsInARow = mountInfo.value("boostsInARow", uint32_t{}),
    .winsSpeedSingle = mountInfo.value("winsSpeedSingle", uint32_t{}),
    .winsSpeedTeam = mountInfo.value("winsSpeedTeam", uint32_t{}),
    .winsMagicSingle = mountInfo.value("winsMagicSingle", uint32_t{}),
    .winsMagicTeam = mountInfo.value("winsMagicTeam", uint32_t{}),
    .totalDistance = mountInfo.value("totalDistance", uint32_t{}),
    .topSpeed = mountInfo.value("topSpeed", uint32_t{}),
    .longestGlideDistance = mountInfo.value("longestGlideDistance", uint32_t{}),
    .participated = mountInfo.value("participated", uint32_t{}),
    .cumulativePrize = mountInfo.value("cumulativePrize", uint32_t{}),
    .biggestPrize = mountInfo.value("biggestPrize", uint32_t{})};
}

void server::FileDataSource::StoreHorse(data::Uid uid, const data::Horse& horse)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _horseDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = horse.uid();
  json["tid"] = horse.tid();
  json["name"] = horse.name();

  nlohmann::json parts;
  parts["skinId"] = horse.parts.skinTid();
  parts["faceId"] = horse.parts.faceTid();
  parts["maneId"] = horse.parts.maneTid();
  parts["tailId"] = horse.parts.tailTid();
  json["parts"] = parts;

  nlohmann::json appearance;
  appearance["scale"] = horse.appearance.scale();
  appearance["legLength"] = horse.appearance.legLength();
  appearance["legVolume"] = horse.appearance.legVolume();
  appearance["bodyLength"] = horse.appearance.bodyLength();
  appearance["bodyVolume"] = horse.appearance.bodyVolume();
  json["appearance"] = appearance;

  nlohmann::json stats;
  stats["agility"] = horse.stats.agility();
  stats["courage"] = horse.stats.courage();
  stats["rush"] = horse.stats.rush();
  stats["endurance"] = horse.stats.endurance();
  stats["ambition"] = horse.stats.ambition();
  json["stats"] = stats;

  nlohmann::json mastery;
  mastery["spurMagicCount"] = horse.mastery.spurMagicCount();
  mastery["jumpCount"] = horse.mastery.jumpCount();
  mastery["slidingTime"] = horse.mastery.slidingTime();
  mastery["glidingDistance"] = horse.mastery.glidingDistance();
  json["mastery"] = mastery;

  nlohmann::json mountCondition;
  mountCondition["stamina"] = horse.mountCondition.stamina();
  mountCondition["charm"] = horse.mountCondition.charm();
  mountCondition["friendliness"] = horse.mountCondition.friendliness();
  mountCondition["injury"] = horse.mountCondition.injury();
  mountCondition["plenitude"] = horse.mountCondition.plenitude();
  mountCondition["bodyDirtiness"] = horse.mountCondition.bodyDirtiness();
  mountCondition["maneDirtiness"] = horse.mountCondition.maneDirtiness();
  mountCondition["tailDirtiness"] = horse.mountCondition.tailDirtiness();
  mountCondition["bodyPolish"] = horse.mountCondition.bodyPolish();
  mountCondition["manePolish"] = horse.mountCondition.manePolish();
  mountCondition["tailPolish"] = horse.mountCondition.tailPolish();
  mountCondition["attachment"] = horse.mountCondition.attachment();
  mountCondition["boredom"] = horse.mountCondition.boredom();
  mountCondition["stopAmendsPoint"] = horse.mountCondition.stopAmendsPoint();
  json["mountCondition"] = mountCondition;

  json["rating"] = horse.rating();
  json["clazz"] = horse.clazz();
  json["clazzProgress"] = horse.clazzProgress();
  json["grade"] = horse.grade();
  json["growthPoints"] = horse.growthPoints();
  
  json["breedingCount"] = horse.breedingCount();
  json["breedingCombo"] = horse.breedingCombo();

  json["type"] = horse.type();
  json["dateOfBirth"] = std::chrono::ceil<std::chrono::seconds>(
    horse.dateOfBirth().time_since_epoch()).count();

  json["tendency"] = horse.tendency();
  json["spirit"] = horse.spirit();

  nlohmann::json potential;
  potential["type"] = horse.potential.type();
  potential["level"] = horse.potential.level();
  potential["value"] = horse.potential.value();
  json["potential"] = potential;

  json["luckState"] = horse.luckState();
  json["fatigue"] = horse.fatigue();
  json["emblem"] = horse.emblemUid();

  nlohmann::json mountInfo;
  mountInfo["boostsInARow"] = horse.mountInfo.boostsInARow();
  mountInfo["winsSpeedSingle"] = horse.mountInfo.winsSpeedSingle();
  mountInfo["winsSpeedTeam"] = horse.mountInfo.winsSpeedTeam();
  mountInfo["winsMagicSingle"] = horse.mountInfo.winsMagicSingle();
  mountInfo["winsMagicTeam"] = horse.mountInfo.winsMagicTeam();
  mountInfo["totalDistance"] = horse.mountInfo.totalDistance();
  mountInfo["topSpeed"] = horse.mountInfo.topSpeed();
  mountInfo["longestGlideDistance"] = horse.mountInfo.longestGlideDistance();
  mountInfo["participated"] = horse.mountInfo.participated();
  mountInfo["cumulativePrize"] = horse.mountInfo.cumulativePrize();
  mountInfo["biggestPrize"] = horse.mountInfo.biggestPrize();
  json["mountInfo"] = mountInfo;

  nlohmann::json ancestorsJson;
  ancestorsJson["father"] = horse.ancestors.father;
  ancestorsJson["mother"] = horse.ancestors.mother;
  json["ancestors"] = ancestorsJson;

  json["lineage"] = horse.lineage();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Horse file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteHorse(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _horseDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Horse file");
}

void server::FileDataSource::CreateItem(data::Item& item)
{
  item.uid = NextUid(_equipmentSequentialUid, "equipment");
  SaveMetadata();
}

void server::FileDataSource::RetrieveItem(data::Uid uid, data::Item& item)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _itemDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Item file", server::util::FileSensitivity::Public);

  item.uid = json.value("uid", data::Uid{});
  item.tid = json.value("tid", data::Tid{});
  item.count = json.value("count", uint32_t{});
  item.duration = std::chrono::seconds(json.value("duration", int64_t{}));
  item.createdAt = data::Clock::time_point(
    std::chrono::seconds(json.value("createdAt", int64_t{})));
}

void server::FileDataSource::StoreItem(data::Uid uid, const data::Item& item)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _itemDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = item.uid();
  json["tid"] = item.tid();
  json["count"] = item.count();
  json["duration"] = item.duration().count();
  json["createdAt"] = std::chrono::ceil<std::chrono::seconds>(
    item.createdAt().time_since_epoch()).count();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Item file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteItem(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _itemDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Item file");
}

void server::FileDataSource::CreateStorageItem(data::StorageItem& item)
{
  item.uid = NextUid(_storageItemSequentialUid, "storageItem");
  SaveMetadata();
}

void server::FileDataSource::RetrieveStorageItem(data::Uid uid, data::StorageItem& storageItem)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _storageItemPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Storage item file", server::util::FileSensitivity::Public);

  storageItem.uid = json.value("uid", data::Uid{});
  storageItem.sender = json.value("sender", std::string{});
  storageItem.message = json.value("message", std::string{});
  storageItem.carrots = json.value("carrots", int32_t{});

  for (const auto& itemJson : json.value("items", nlohmann::json::array()))
  {
    storageItem.items().emplace_back(data::StorageItem::Item{
      .tid = itemJson.value("tid", data::Tid{}),
      .count = itemJson.value("count", uint32_t{}),
      .duration = std::chrono::seconds(
        itemJson.value("duration", int64_t{})),});
  }

  storageItem.checked = json.value("checked", bool{});
  storageItem.duration = std::chrono::seconds(
    json.value("duration", int64_t{}));
  storageItem.createdAt = data::Clock::time_point(std::chrono::seconds(
    json.value("createdAt", int64_t{})));

  // Shop data
  storageItem.goodsSq = json.value("goodsSq", uint32_t{});
  storageItem.priceId = json.value("priceId", uint32_t{});
}

void server::FileDataSource::StoreStorageItem(data::Uid uid, const data::StorageItem& storageItem)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _storageItemPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = storageItem.uid();
  json["sender"] = storageItem.sender();
  json["message"] = storageItem.message();
  json["carrots"] = storageItem.carrots();

  auto& itemsJson = json["items"];
  for (const auto& item : storageItem.items())
  {
    nlohmann::json itemJson;
    itemJson["tid"] = item.tid;
    itemJson["count"] = item.count;
    itemJson["duration"] = item.duration.count();

    itemsJson.emplace_back(itemJson);
  }

  json["checked"] = storageItem.checked();
  json["createdAt"] = std::chrono::ceil<std::chrono::seconds>(
    storageItem.createdAt().time_since_epoch()).count();
  json["duration"] = storageItem.duration().count();

  // Shop data
  json["goodsSq"] = storageItem.goodsSq();
  json["priceId"] = storageItem.priceId();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Storage item file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteStorageItem(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _storageItemPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Storage item file");
}

void server::FileDataSource::CreateEgg(data::Egg& egg)
{
  egg.uid = NextUid(_eggSequentialUid, "egg");
  SaveMetadata();
}

void server::FileDataSource::RetrieveEgg(data::Uid uid, data::Egg& egg)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _eggDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Egg file", server::util::FileSensitivity::Public);

  egg.uid = json.value("uid", data::Uid{});
  egg.itemUid = json.value("itemUid", data::Uid{});
  egg.itemTid = json.value("itemTid", data::Tid{});

  egg.incubatedAt = data::Clock::time_point(
    std::chrono::seconds(
      json.value("incubatedAt", uint64_t{})));
  egg.incubatorSlot = json.value("incubatorSlot", uint32_t{});
  egg.boostsUsed = json.value("boostsUsed", uint32_t{});
}

void server::FileDataSource::StoreEgg(data::Uid uid, const data::Egg& egg)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _eggDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = egg.uid();
  json["itemUid"] = egg.itemUid();
  json["itemTid"] = egg.itemTid();
  json["incubatedAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    egg.incubatedAt().time_since_epoch()).count();
  json["incubatorSlot"] = egg.incubatorSlot();
  json["boostsUsed"] = egg.boostsUsed();
  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Egg file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteEgg(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _eggDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Egg file");
}

void server::FileDataSource::CreatePet(data::Pet& pet)
{
  pet.uid = NextUid(_petSequentialUid, "pet");
  SaveMetadata();
}

void server::FileDataSource::RetrievePet(data::Uid uid, data::Pet& pet)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _petDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Pet file", server::util::FileSensitivity::Public);

  pet.uid = json.value("uid", data::Uid{});
  pet.itemUid = json.value("itemUid", data::Uid{});
  pet.petId = json.value("petId", data::Uid{});
  pet.name = json.value("name", std::string{});
  pet.birthDate = data::Clock::time_point(std::chrono::seconds(
    json.value("birthDate", uint64_t{})));
}

void server::FileDataSource::StorePet(data::Uid uid, const data::Pet& pet)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _petDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = pet.uid();
  json["itemUid"] = pet.itemUid();
  json["petId"] = pet.petId();
  json["name"] = pet.name();
  json["birthDate"] = std::chrono::duration_cast<std::chrono::seconds>(
    pet.birthDate().time_since_epoch()).count();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Pet file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeletePet(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _petDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Pet file");
}

void server::FileDataSource::CreateHousing(data::Housing& housing)
{
  housing.uid = NextUid(_housingSequentialUid, "housing");
  SaveMetadata();
}

void server::FileDataSource::RetrieveHousing(data::Uid uid, data::Housing& housing)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _housingDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Housing file", server::util::FileSensitivity::Public);
  housing.uid = json.value("uid", data::Uid{});
  housing.housingId = json.value("housingId", uint32_t{});
  housing.expiresAt = data::Clock::time_point(
    std::chrono::seconds(json.value("expiresAt", uint64_t{})));
  housing.durability = json.value("durability", uint32_t{});
}

void server::FileDataSource::StoreHousing(data::Uid uid, const data::Housing& housing)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _housingDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = housing.uid();
  json["housingId"] = housing.housingId();
  json["expiresAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    housing.expiresAt().time_since_epoch()).count();
  json["durability"] = housing.durability();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Housing file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteHousing(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _housingDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Housing file");
}

void server::FileDataSource::CreateGuild(data::Guild& guild)
{
  guild.uid = NextUid(_guildSequentialId, "guild");
  SaveMetadata();
}

void server::FileDataSource::RetrieveGuild(data::Uid uid, data::Guild& guild)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _guildDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Guild file", server::util::FileSensitivity::Public);

  guild.uid = json.value("uid", data::Uid{});
  guild.name = json.value("name", std::string{});
  guild.description = json.value("description", std::string{});
  guild.owner = json.value("owner", data::Uid{});
  guild.officers = json.value("officers", std::vector<data::Uid>{});
  guild.members = json.value("members", std::vector<data::Uid>{});

  guild.rank = json.value("rank", uint32_t{});
  guild.totalWins = json.value("totalWins", uint32_t{});
  guild.totalLosses = json.value("totalLosses", uint32_t{});
  guild.seasonalWins = json.value("seasonalWins", uint32_t{});
  guild.seasonalLosses = json.value("seasonalLosses", uint32_t{});
}

void server::FileDataSource::StoreGuild(data::Uid uid, const data::Guild& guild)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _guildDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = guild.uid();
  json["name"] = guild.name();
  json["description"] = guild.description();
  json["owner"] = guild.owner();
  json["officers"] = guild.officers();
  json["members"] = guild.members();

  json["rank"] = guild.rank();
  json["totalWins"] = guild.totalWins();
  json["totalLosses"] = guild.totalLosses();
  json["seasonalWins"] = guild.seasonalWins();
  json["seasonalLosses"] = guild.seasonalLosses();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Guild file",
    server::util::FileSensitivity::Public);

  // ★ИНДЕКС ПОСЛЕ УСПЕШНОЙ ЗАПИСИ, как у персонажей и аккаунтов: бросок
  // оставляет диск в прежнем состоянии, и индекс обязан остаться согласованным
  // с диском, а не с намерением.
  IndexGuildName(uid, guild.name());
}

void server::FileDataSource::DeleteGuild(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _guildDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Guild file");
  ForgetGuildName(uid);
}

bool server::FileDataSource::IsGuildNameUnique(const std::string_view& name)
{
  // ★ИНДЕКС ВМЕСТО ОБХОДА КАТАЛОГА (LOA-fix R73-13, правка ревью, итерация 6).
  //
  // Прежняя редакция ЛИСТАЛА каталог гильдий и разбирала каждый файл на каждый
  // пакет создания гильдии. Итерация 5 сделала этот обход безопасным (общий
  // список вместо бросающего итератора), но безопасный обход — всё ещё обход:
  // проверка стоит ДО списания 3000 морковок, поэтому повторение занятого имени
  // покупало полный проход по файловой системе за ноль. Класс закрывается тем
  // же способом, что у персонажей и аккаунтов, — индексом, а не третьим частным
  // случаем: ответ теперь стоит один хеш, независимо от числа гильдий.
  //
  // ★СТРУКТУРНЫЙ ГЕЙТ ДО ВСЕГО — до ключа, до хеша, до замка (правка ревью,
  // итерация 7; директива ведущего требовала провести создание гильдии через
  // NameGuard И индекс, а итерация 6 поставила только индекс).
  //
  // Провод отдаёт до ~8190 байт (`Stream.cpp` при потолке `MaxCommandDataSize`),
  // а хранимое имя короче на два порядка. Сегодня перед этим вызовом стоит
  // `locale::IsNameValid(command.name)` с потолком 18 байт — но это ОДИН
  // вызывающий, и свойство «имя с провода не оплачивается работой хранилища»
  // держится его вежливостью, а не построением. Второй вызывающий,
  // написанный по образцу соседей (`RetrieveCharacterUidByName`,
  // `IsUserNameUnique` — оба со своим гейтом), унаследовал бы отсутствие
  // гейта. Правило живёт в хранилище, а не в перечне вызывающих.
  //
  // ★ПОТОЛОК ИЗ ИНДЕКСА, А НЕ КОНСТАНТА: гейт, который строже индекса, отнял
  // бы у гильдии, названной до появления валидатора, возможность быть
  // спрошенной. Пол — тот же `kMaxStoredNameBytes`, что у персонажей.
  //
  // ★ОТКАЗ ЧИТАЕТСЯ КАК «ЗАНЯТО», А НЕ КАК «СВОБОДНО», и направление здесь
  // противоположно `IsUserNameUnique` НЕ по недосмотру: там ответ идёт в
  // ПОИСК (staff-команда скажет «такого нет»), а здесь — в СОЗДАНИЕ. Имя,
  // которое физически не может лежать на диске, обязано отказать в создании,
  // иначе гейт превратился бы в способ пройти проверку уникальности.
  if (not server::util::IsStorableNameShaped(
    name, _guildNameCeiling.load(std::memory_order::relaxed)))
  {
    _rejectedNameLookups.fetch_add(1, std::memory_order::relaxed);
    return false;
  }

  // Сравнение по-прежнему ASCII-регистронезависимое: ключ индекса и есть имя в
  // нижнем ASCII-регистре, ровно то, что делал прежний `equalsIgnoreCase`.
  const auto key = server::util::AsciiLowerKey(name);

  // ★ТОТ ЖЕ ОДИН СНИМОК, ЧТО У ПЕРСОНАЖЕЙ (правка ревью, итерация 9). Здесь
  // раздельного чтения не было и до правки, но помощник один на все три места
  // намеренно: пока правило записано в трёх телах, четвёртое место, написанное
  // по образцу соседей, унаследует ту форму, которую увидит.
  auto answer = ReadNameIndexAnswer(
    _guildNameIndexMutex, _guildNameToUid, _guildNameIndexComplete, key);
  if (answer.uid != data::InvalidUid)
    return false;
  if (answer.complete)
    return true;

  // ★ИНДЕКС, КОТОРЫЙ ВИДЕЛ НЕ ВСЁ, НЕ ОТВЕЧАЕТ «СВОБОДНО» (правка ревью,
  // итерация 7). Перестройка МОЛЧА пропускала нечитаемый файл, битый JSON и имя
  // файла, из которого не читается uid, — и публиковала набор, выглядящий
  // полным. Гильдия, чей файл на старте оказался временно нечитаемым, отдавала
  // своё имя следующему желающему НА ВСЁ ВРЕМЯ РАБОТЫ сервера. Сначала одна
  // попытка починиться (не чаще раза в две секунды), и только потом ответ; если
  // индекс всё ещё неполон — «занято», то есть отказ в создании, а не выдача
  // чужого имени.
  ReconcileGuildNameIndexIfBroken();
  answer = ReadNameIndexAnswer(
    _guildNameIndexMutex, _guildNameToUid, _guildNameIndexComplete, key);
  if (answer.uid != data::InvalidUid)
    return false;
  return answer.complete;
}

void server::FileDataSource::MarkGuildNameIndexBroken(
  const std::string_view what, const std::string_view detail) noexcept
{
  // ★ФЛАГ АТОМАРНЫЙ ИМЕННО РАДИ ЭТОГО ПУТИ: сюда приходят с отказа (нехватка
  // памяти), и требовать здесь замок значило бы уметь НЕ объявить индекс
  // сломанным ровно тогда, когда он сломан.
  _guildNameIndexComplete.store(false, std::memory_order::relaxed);
  try
  {
    server::util::QuietLogError(
      "Guild name index: {} ({}); every guild name reads as taken until the "
      "index is rebuilt from disk", what, detail);
  }
  catch (...)
  {
    // Отчёт о беде не имеет права стать второй бедой.
  }
}

void server::FileDataSource::MarkCharacterNameIndexBroken(
  const std::string_view what, const std::string_view detail) noexcept
{
  _characterNameIndexComplete.store(false, std::memory_order::relaxed);
  try
  {
    server::util::QuietLogError(
      "Character name index: {} ({}); every character name reads as taken until "
      "the index is rebuilt from disk", what, detail);
  }
  catch (...)
  {
  }
}

bool server::FileDataSource::ReconcileGuildNameIndexIfBroken()
{
  {
    const std::shared_lock indexLock(_guildNameIndexMutex);
    if (_guildNameIndexComplete)
      return false;
  }
  if (not ClaimNameIndexRetry(_guildIndexLastRetry, kBrokenNameIndexRetryGap))
    return false;

  try
  {
    RebuildGuildNameIndex();
  }
  catch (const std::exception& x)
  {
    // ★БРОСОК ЗДЕСЬ ГАСИТСЯ, И ЭТО НЕ ПРОТИВОРЕЧИТ СТАРТУ. На старте оборванный
    // обход обязан остановить сервер: там остановка возможна и честна. В
    // рантайме этот же бросок разорвал бы пакет создания гильдии, а индекс и
    // так остаётся объявленным неполным — то есть ответ уже безопасный.
    server::util::QuietLogError(
      "Guild name index: the rebuild attempt failed ({}); every guild name "
      "keeps reading as taken", x.what());
    return false;
  }
  return true;
}

bool server::FileDataSource::ReconcileCharacterNameIndexIfBroken()
{
  {
    const std::shared_lock indexLock(_characterNameIndexMutex);
    if (_characterNameIndexComplete)
      return false;
  }
  if (not ClaimNameIndexRetry(_characterIndexLastRetry, kBrokenNameIndexRetryGap))
    return false;

  try
  {
    RebuildCharacterNameIndex();
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError(
      "Character name index: the rebuild attempt failed ({}); every character "
      "name keeps reading as taken", x.what());
    return false;
  }
  return true;
}

void server::FileDataSource::RebuildGuildNameIndex()
{
  const std::unique_lock indexLock(_guildNameIndexMutex);
  // ★ФЛАГ ПОЛНОТЫ СНИМАЕТСЯ ПЕРВЫМ ДЕЙСТВИЕМ (правка ревью, итерация 7): бросок
  // или отказ посреди обхода обязан оставить индекс объявленным НЕПОЛНЫМ, а не
  // «таким, каким он был до этого».
  _guildNameIndexComplete = false;
  _guildNameToUid.clear();
  _guildUidToName.clear();

  //! Сколько записей мы НЕ СМОГЛИ прочитать. Ноль — и только ноль — даёт право
  //! отвечать «имя свободно».
  std::size_t unresolved = 0;
  std::size_t duplicates = 0;

  const auto listing = server::util::ListRegularFiles(_guildDataPath);
  if (listing.incomplete)
  {
    // ★ОБОРВАННЫЙ ОБХОД НА СТАРТЕ ФАТАЛЕН, как и у персонажей: перестройка
    // зовётся из `Initialize`, где остановка возможна и честна. Из рантайма её
    // зовёт `ReconcileGuildNameIndexIfBroken`, который этот бросок гасит и
    // оставляет индекс неполным, то есть отвечающим «занято».
    server::util::QuietLogError(
      "Guild name index: the scan of '{}' did not finish; refusing to start "
      "with taken guild names readable as free",
      server::util::LogPath(_guildDataPath));

    throw std::runtime_error(
      std::format(
        "Guild name index scan of '{}' did not finish",
        server::util::LogPath(_guildDataPath)));
  }

  if (not listing.refusedSymlinks.empty())
  {
    // ★ОТВЕРГНУТАЯ ССЫЛКА — ЭТО «НЕ ВИДЕЛИ», А НЕ «ТАМ ПУСТО» (правка ревью,
    // итерация 7). Прежде это была только строка в логе, а имя под ссылкой
    // читалось как свободное.
    unresolved += listing.refusedSymlinks.size();
    server::util::QuietLogWarn(
      "Guild name index: {} entry(ies) in '{}' are symbolic links and were "
      "refused; the names they may carry are unknown and read as taken",
      listing.refusedSymlinks.size(), server::util::LogPath(_guildDataPath));
  }

  for (const auto& filePath : listing.files)
  {
    // Тот же фильтр, что у прежнего обхода (R58-9): осиротевший `7.json.tmp` не
    // имеет права занять имя.
    if (filePath.extension() != ".json")
      continue;

    // ★ЛИЧНОСТЬ ЗАПИСИ БЕРЁТСЯ ИЗ ИМЕНИ ФАЙЛА, А НЕ ИЗ `json["uid"]` (правка
    // ревью, итерация 7). `StoreGuild` и `DeleteGuild` адресуют файл именем;
    // индекс, взявший uid из содержимого, отвечал бы про ДРУГУЮ запись:
    // `7.json` с полем `uid: 8` делал `DeleteGuild(7)` неспособным освободить
    // имя, а два файла с одним полем `uid` освобождали живое имя.
    const auto uid = ParseRecordUid(filePath.stem().string());
    if (not uid)
    {
      ++unresolved;
      server::util::QuietLogWarn(
        "Guild file '{}' is not named after a record identifier; the index "
        "refuses to guess whose name it carries",
        server::util::LogPath(filePath));
      continue;
    }

    const auto read = server::util::ReadManagedFile(
      filePath, server::util::FileSensitivity::Public);
    if (read.status != server::util::ManagedReadStatus::Ok)
    {
      ++unresolved;
      continue;
    }

    std::string existingName;
    try
    {
      const auto json = nlohmann::json::parse(read.content);
      existingName = json.value("name", std::string{});
    }
    catch (const std::exception& x)
    {
      // ★БИТЫЙ ФАЙЛ БОЛЬШЕ НЕ «ПРОСТО ПРОПУСКАЕТСЯ» (правка ревью, итерация 7).
      // Пропуск публиковал индекс, выглядящий полным, и имя этой гильдии
      // становилось свободным до перезапуска.
      server::util::QuietLogWarn(
        "Guild file '{}' is unreadable ({}) and was skipped while building the "
        "guild name index", server::util::LogPath(filePath), x.what());
      ++unresolved;
      continue;
    }

    if (existingName.empty())
    {
      ++unresolved;
      continue;
    }

    auto key = server::util::AsciiLowerKey(existingName);

    // Один uid живёт ровно в одном списке (см. индекс персонажей).
    const auto alreadyIndexed = _guildUidToName.find(*uid);
    if (alreadyIndexed != _guildUidToName.end())
    {
      if (alreadyIndexed->second == key)
        continue;
      DetachNameKey(_guildNameToUid, alreadyIndexed->second, *uid);
    }

    const std::size_t collisions =
      AttachNameKey(_guildNameToUid, key, *uid);
    // ★ПОТОЛОК ГЕЙТА МЕРЯЕТСЯ ТЕМ, ЧТО ЛЕЖИТ НА ДИСКЕ, а не сегодняшним
    // валидатором: гильдия, названная до появления `IsNameValid`, обязана
    // остаться спрашиваемой (тот же вывод, что у персонажей, — итерация 1).
    RaiseNameCeiling(_guildNameCeiling, existingName.size());
    if (collisions > 1)
      ++duplicates;
    _guildUidToName[*uid] = std::move(key);
  }

  _guildNameIndexComplete = unresolved == 0;
  // Момент попытки переставляется и здесь: старт — это тоже попытка, и сразу
  // после него повторять обход незачем.
  _guildIndexLastRetry.store(
    std::chrono::steady_clock::now(), std::memory_order::relaxed);

  server::util::QuietLogInfo(
    "Guild name index: {} names indexed, {} files unresolved, {} duplicates",
    _guildNameToUid.size(), unresolved, duplicates);

  if (not _guildNameIndexComplete)
  {
    server::util::QuietLogError(
      "Guild name index: {} guild file(s) in '{}' could not be read; until "
      "they can be, EVERY guild name reads as taken rather than free",
      unresolved, server::util::LogPath(_guildDataPath));
  }
}

void server::FileDataSource::IndexGuildName(
  const data::Uid uid, const std::string& name)
{
  // ★ПУСТОЕ ИМЯ СНИМАЕТ ЗАПИСЬ, А НЕ ЗАВОДИТ КЛЮЧ "" — перестройка такие файлы
  // пропускает, и без этой строки рантайм и диск разошлись бы после рестарта.
  if (name.empty())
  {
    ForgetGuildName(uid);
    return;
  }

  // ★НЕУДАЧА ПОДДЕРЖАНИЯ ИНДЕКСА ОБЪЯВЛЯЕТ ИНДЕКС НЕПОЛНЫМ, А НЕ ОСТАВЛЯЕТ ЕГО
  // ПОЛУПЕРЕПИСАННЫМ (правка ревью, итерация 7).
  //
  // Нас зовут ПОСЛЕ успешной записи файла. Переименование снимало старый ключ
  // ПЕРЕД тем, как выделить новое ведро, новый вектор и обратную запись:
  // `bad_alloc` посередине оставлял имя, лежащее на диске, ВНЕ индекса — то
  // есть занятое имя читалось как свободное, — либо пустой ключ, который ни
  // один `ForgetGuildName` уже не снимет. Готовить новое состояние до снятия
  // старого здесь недостаточно: бросить умеет и вставка. Поэтому отказ
  // обрабатывается ТАК ЖЕ, как неполный обход, — весь индекс объявляется
  // неполным, ответом становится «занято», а следующая попытка пересобирает его
  // с диска. Индекс — кэш диска, и его отказ не имеет права соврать про диск.
  try
  {
    auto key = server::util::AsciiLowerKey(name);
    RaiseNameCeiling(_guildNameCeiling, name.size());
    const std::unique_lock indexLock(_guildNameIndexMutex);
    const auto previous = _guildUidToName.find(uid);
    if (previous != _guildUidToName.end())
    {
      if (previous->second == key)
        return;                                 // имя не менялось
      DetachNameKey(_guildNameToUid, previous->second, uid);
    }
    AttachNameKey(_guildNameToUid, key, uid);
    _guildUidToName[uid] = std::move(key);
  }
  catch (const std::exception& x)
  {
    MarkGuildNameIndexBroken("a guild name could not be indexed", x.what());
  }
}

void server::FileDataSource::ForgetGuildName(const data::Uid uid)
{
  const std::unique_lock indexLock(_guildNameIndexMutex);
  const auto previous = _guildUidToName.find(uid);
  if (previous == _guildUidToName.end())
    return;
  // Снятие одной гильдии поднимает следующую, носящую то же имя, а не гасит имя.
  DetachNameKey(_guildNameToUid, previous->second, uid);
  _guildUidToName.erase(previous);
}

void server::FileDataSource::CreateSettings(data::Settings& settings)
{
  settings.uid = NextUid(_settingsSequentialId, "settings");
  SaveMetadata();
}

void server::FileDataSource::RetrieveSettings(data::Uid uid, data::Settings& settings)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _settingsDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Settings file", server::util::FileSensitivity::Public);
  settings.uid = json.value("uid", data::Uid{});

  settings.age = json.value("age", uint32_t{});
  settings.hideAge = json.value("hideGenderAndAge", bool{});

  // Keyboard bindings
  {
    const auto& keyboardJson = json.value("keyboard", nlohmann::json::object());
    const auto& keyboardBindingsJson = keyboardJson.value("bindings", nlohmann::json::array());
    if (not keyboardBindingsJson.empty())
    {
      auto& keyboardBindings = settings.keyboardBindings().emplace();

      for (const auto& keyboardBindingJson : keyboardBindingsJson)
      {
        keyboardBindings.emplace_back(data::Settings::Option{
          .primaryKey = keyboardBindingJson.value("primaryKey", uint32_t{}),
          .type = keyboardBindingJson.value("type", uint32_t{}),
          .secondaryKey = keyboardBindingJson.value("secondaryKey", uint32_t{})
        });
      }
    }
  }

  // Gamepad bindings
  {
    const auto& gamepadJson = json.value("gamepad", nlohmann::json::object());
    const auto& gamepadBindingsJson = gamepadJson.value("bindings", nlohmann::json::array());
    if (not gamepadBindingsJson.empty())
    {
      auto& gamepadBindings = settings.gamepadBindings().emplace();

      for (const auto& gamepadBindingJson : gamepadBindingsJson)
      {
        gamepadBindings.emplace_back(data::Settings::Option{
          .primaryKey = gamepadBindingJson.value("primaryButton", uint32_t{}),
          .type = gamepadBindingJson.value("type", uint32_t{}),
          .secondaryKey = gamepadBindingJson.value("secondaryButton", uint32_t{})
        });
      }
    }
  }

  if (json.contains("macros"))
  {
    const auto& macrosJson = json["macros"];
    settings.macros().emplace() = macrosJson.get<std::array<std::string, 8>>();
  }
}

void server::FileDataSource::StoreSettings(data::Uid uid, const data::Settings& settings)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _settingsDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = settings.uid();

  json["age"] = settings.age();
  json["hideGenderAndAge"] = settings.hideAge();

  // Keyboard bindings
  {
    auto& keyboardJson = json["keyboard"];
    auto& bindings = keyboardJson["bindings"];

    if (settings.keyboardBindings())
    {
      for (auto& bindingRecord : settings.keyboardBindings().value())
      {
        auto& bindingJson = bindings.emplace_back();
        bindingJson["type"] = bindingRecord.type;
        bindingJson["primaryKey"] = bindingRecord.primaryKey;
        bindingJson["secondaryKey"] = bindingRecord.secondaryKey;
      }
    }
  }

  // Gamepad bindings
  {
    auto& gamepadJson = json["gamepad"];
    auto& bindings = gamepadJson["bindings"];

    if (settings.gamepadBindings())
    {
      for (auto& bindingRecord : settings.gamepadBindings().value())
      {
        auto& bindingJson = bindings.emplace_back();
        bindingJson["type"] = bindingRecord.type;
        bindingJson["primaryButton"] = bindingRecord.primaryKey;
        bindingJson["secondaryButton"] = bindingRecord.secondaryKey;
      }
    }
  }

  // Macros
  if (settings.macros())
  {
    json["macros"] = settings.macros().value();
  }

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Settings file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteSettings(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _settingsDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Settings file");
}

void server::FileDataSource::CreateDailyQuestGroup(data::DailyQuestGroup& group)
{
  group.uid = NextUid(_dailyQuestGroupSequentialId, "dailyQuestGroup");
  SaveMetadata();
}

void server::FileDataSource::RetrieveDailyQuestGroup(data::Uid uid, data::DailyQuestGroup& group)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _dailyQuestGroupDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Daily quest group file", server::util::FileSensitivity::Public);
  group.uid          = json.value("uid", data::Uid{});
  group.rewardId     = json.value("rewardId", uint8_t{});
  group.rewardType   = json.value("rewardType", uint8_t{});
  group.rewardPoints = json.value("rewardPoints", uint32_t{0});
  // LOA-fix (batch1 task3): день последнего сброса дейликов; старые файлы без
  // ключа → 0 (никогда) → первый вход после деплоя сбросит квесты и carrotsClaimed.
  group.lastResetDate = json.value("lastResetDate", uint32_t{0});
  // LOA-fix (R17-cap, quest-batch-2, #8): daily class-exp cap counter. Old files
  // without the key load as 0 (uncapped start after deploy — one-time, self-heals on
  // the first daily reset).
  group.dailyClassExpGranted = json.value("dailyClassExpGranted", uint32_t{0});
  // ★МИГРАЦИЯ (Codex-CHANGES #8 iter2): для legacy-записи (ключа нет) сидируем
  // dailyClassExpResetDate из ПЕРСИСТНУТОГО lastResetDate ПРЯМО ЗДЕСЬ, при
  // десериализации — ДО того как любой runtime quest-reset (login/ранч-вход)
  // продвинет lastResetDate. Под старой системой счётчик сбрасывался вместе с
  // lastResetDate, поэтому lastResetDate из ЭТОГО ЖЕ json = истинная as-of-дата
  // счётчика. Runtime-seed сломался бы: счётчик теперь переживает quest-reset
  // (R42-7/8), значит к первому spend'у lastResetDate мог уже стать today при
  // counter=вчерашний 6650 → underpay. Load-seed это исключает (одна точка,
  // до всякого runtime-мутатора). Sync после этого — простой rollover-чек.
  group.dailyClassExpResetDate = json.value(
    "dailyClassExpResetDate", json.value("lastResetDate", uint32_t{0}));
  // LOA-fix (F2, quest-batch-1): «награда дня выдана». Старые файлы без ключа →
  // false (награда за сегодня ещё не забрана). Принимаем и bool, и легаси-int,
  // ровно как carrotsClaimed выше.
  if (const auto claimedIt = json.find("dailyRewardClaimed"); claimedIt != json.end())
    group.dailyRewardClaimed =
      claimedIt->is_boolean() ? claimedIt->get<bool>() : (claimedIt->get<int>() != 0);
  else
    group.dailyRewardClaimed = false;
  // Support both boolean `true`/`false` and legacy integer `1`/`0` representations.
  if (const auto it = json.find("carrotsClaimed"); it != json.end())
    group.carrotsClaimed = it->is_boolean() ? it->get<bool>() : (it->get<int>() != 0);
  else
    group.carrotsClaimed = false;

  std::array<data::DailyQuestEntry, 3> quests{};
  const auto& questsJson = json.value("quests", nlohmann::json::array());
  for (size_t i = 0; i < questsJson.size() && i < 3; ++i)
  {
    quests[i].questId  = questsJson[i].value("questId", uint16_t{});
    quests[i].progress = questsJson[i].value("progress", uint32_t{});
  }
  group.quests = quests;
}

void server::FileDataSource::StoreDailyQuestGroup(data::Uid uid, const data::DailyQuestGroup& group)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _dailyQuestGroupDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"]          = group.uid();
  json["rewardId"]     = group.rewardId();
  json["rewardType"]   = group.rewardType();
  json["rewardPoints"] = group.rewardPoints();
  json["lastResetDate"] = group.lastResetDate();
  json["dailyRewardClaimed"] = static_cast<bool>(group.dailyRewardClaimed());
  json["dailyClassExpGranted"] = group.dailyClassExpGranted();
  json["dailyClassExpResetDate"] = group.dailyClassExpResetDate();
  json["carrotsClaimed"] = static_cast<bool>(group.carrotsClaimed());

  nlohmann::json questsJson = nlohmann::json::array();
  for (const auto& entry : group.quests())
  {
    questsJson.push_back({
      {"questId",  entry.questId},
      {"progress", entry.progress}
    });
  }
  json["quests"] = questsJson;
  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Daily quest group file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteDailyQuestGroup(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _dailyQuestGroupDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Daily quest group file");
}

void server::FileDataSource::CreateMail(data::Mail& mail)
{
  mail.uid = NextUid(_mailSequentialId, "mail");
  SaveMetadata();
}

void server::FileDataSource::RetrieveMail(data::Uid uid, data::Mail& mail)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _mailDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Mail file", server::util::FileSensitivity::Public);
  mail.uid = json.value("uid", data::Uid{});
  mail.from = json.value("from", data::Uid{});
  mail.to = json.value("to", data::Uid{});

  mail.isRead = json.value("isRead", bool{});
  mail.isDeleted = json.value("isDeleted", bool{});

  mail.type = json.value("type", data::Mail::MailType{});
  mail.claimUid = json.value("claimUid", data::Uid{});

  mail.createdAt = data::Clock::time_point(
    std::chrono::seconds(
      json.value("createdAt", uint64_t{})));
  mail.body = json.value("body", std::string{});
}

void server::FileDataSource::StoreMail(data::Uid uid, const data::Mail& mail)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _mailDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = mail.uid();
  json["from"] = mail.from();
  json["to"] = mail.to();

  json["isRead"] = mail.isRead();
  json["isDeleted"] = mail.isDeleted();

  json["type"] = mail.type();
  json["claimUid"] = mail.claimUid();

  json["createdAt"] = std::chrono::duration_cast<
    std::chrono::seconds>(
      mail.createdAt().time_since_epoch()).count();
  json["body"] = mail.body();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Mail file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteMail(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _mailDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Mail file");
}

void server::FileDataSource::CreateQuest(data::Quest& quest)
{
  quest.uid = NextUid(_questSequentialId, "quest");
  SaveMetadata();
}

void server::FileDataSource::RetrieveQuest(data::Uid uid, data::Quest& quest)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _questDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Quest file", server::util::FileSensitivity::Public);
  quest.uid         = json.value("uid", data::Uid{});
  quest.questId     = json.value("questId", uint32_t{});
  quest.isCompleted = json.value("isCompleted", data::Quest::Status{});
  quest.progress    = json.value("progress", uint32_t{});
}

void server::FileDataSource::StoreQuest(data::Uid uid, const data::Quest& quest)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _questDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"]         = quest.uid();
  json["questId"]     = quest.questId();
  json["isCompleted"] = static_cast<uint32_t>(quest.isCompleted());
  json["progress"]    = quest.progress();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Quest file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteQuest(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _questDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Quest file");
}

void server::FileDataSource::CreateStallion(data::Stallion& stallion)
{
  stallion.uid = NextUid(_stallionSequentialUid, "stallion");
  SaveMetadata();
}

void server::FileDataSource::RetrieveStallion(data::Uid uid, data::Stallion& stallion)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _stallionDataPath, std::format("{}", uid));

  const auto json = ReadManagedJson(
    dataFilePath, "Stallion file", server::util::FileSensitivity::Public);
  stallion.uid() = json.value("uid", data::InvalidUid);
  stallion.horseUid() = json.value("horseUid", data::InvalidUid);
  stallion.ownerUid() = json.value("ownerUid", data::InvalidUid);
  stallion.breedingCharge() = json.value("breedingCharge", data::InvalidUid);
  stallion.timesMated() = json.value("timesMated", uint32_t{0});
  stallion.registeredAt() = data::Clock::time_point(
    std::chrono::seconds(json.value("registeredAt", int64_t{0})));
  stallion.expiresAt() = data::Clock::time_point(
    std::chrono::seconds(json.value("expiresAt", int64_t{0})));
}

void server::FileDataSource::StoreStallion(data::Uid uid, const data::Stallion& stallion)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _stallionDataPath, std::format("{}", uid));

  nlohmann::json json;
  json["uid"] = stallion.uid();
  json["horseUid"] = stallion.horseUid();
  json["ownerUid"] = stallion.ownerUid();
  json["breedingCharge"] = stallion.breedingCharge();
  json["timesMated"] = stallion.timesMated();
  json["registeredAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    stallion.registeredAt().time_since_epoch()).count();
  json["expiresAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    stallion.expiresAt().time_since_epoch()).count();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Stallion file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteStallion(data::Uid uid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _stallionDataPath, std::format("{}", uid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Stallion file");
}

std::vector<server::data::Uid> server::FileDataSource::ListRegisteredStallions()
{
  std::vector<data::Uid> stallionUids;

  // ★ОБХОД ЧЕРЕЗ ОБЩИЙ СПИСОК (правка ревью, итерация 5). Прежняя редакция
  // ходила `directory_iterator`, и оба её шага БРОСАЛИ: продвижение итератора и
  // `is_regular_file()` без `error_code` (последний — ещё и ПО ССЫЛКЕ). Эта
  // функция зовётся при инициализации рынка разведения, то есть самоссылающийся
  // `stallions/x.json` мешал бы подняться службе ранчо. Ссылка не запись, и
  // ронять из-за неё старт нечем.
  const auto listing = server::util::ListRegularFiles(_stallionDataPath);
  if (listing.incomplete)
  {
    server::util::QuietLogWarn(
      "Registered stallions: the scan of '{}' did not finish; the returned list "
      "is incomplete", server::util::LogPath(_stallionDataPath));
  }
  if (not listing.refusedSymlinks.empty())
  {
    server::util::QuietLogWarn(
      "Registered stallions: {} entry(ies) in '{}' are symbolic links and were "
      "refused", listing.refusedSymlinks.size(), server::util::LogPath(_stallionDataPath));
  }

  for (const auto& filePath : listing.files)
  {
    if (filePath.extension() != ".json")
      continue;

    try
    {
      // Extract stallion UID from filename (e.g., "123.json" -> 123)
      data::Uid stallionUid = std::stoul(filePath.stem().string());
      stallionUids.push_back(stallionUid);
    }
    catch (const std::exception&)
    {
      // Silently skip invalid filenames
    }
  }

  return stallionUids;
}

void server::FileDataSource::CreateReward(data::Reward& reward)
{
  reward.claimUid = NextUid(_rewardSequentialUid, "reward");
  SaveMetadata();
}

void server::FileDataSource::RetrieveReward(data::Uid claimUid, data::Reward& reward)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _rewardDataPath, std::format("{}", claimUid));

  const auto json = ReadManagedJson(
    dataFilePath, "Reward file", server::util::FileSensitivity::Public);
  reward.claimUid() = json.value("claimUid", data::InvalidUid);
  reward.characterUid() = json.value("characterUid", data::InvalidUid);
  reward.type() = static_cast<data::Reward::Type>(json.value("type", uint32_t{0}));
  reward.carrots() = json.value("carrots", uint32_t{0});
  reward.isClaimed() = json.value("isClaimed", false);
  reward.createdAt() = data::Clock::time_point(
    std::chrono::seconds(json.value("createdAt", int64_t{0})));
  reward.claimedAt() = data::Clock::time_point(
    std::chrono::seconds(json.value("claimedAt", int64_t{0})));
}

void server::FileDataSource::StoreReward(data::Uid claimUid, const data::Reward& reward)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _rewardDataPath, std::format("{}", claimUid));

  nlohmann::json json;
  json["claimUid"] = reward.claimUid();
  json["characterUid"] = reward.characterUid();
  json["type"] = static_cast<uint32_t>(reward.type());
  json["carrots"] = reward.carrots();
  json["isClaimed"] = reward.isClaimed();
  json["createdAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    reward.createdAt().time_since_epoch()).count();
  json["claimedAt"] = std::chrono::duration_cast<std::chrono::seconds>(
    reward.claimedAt().time_since_epoch()).count();

  server::util::WriteFileAtomically(
    dataFilePath, json.dump(2), "Reward file",
    server::util::FileSensitivity::Public);
}

void server::FileDataSource::DeleteReward(data::Uid claimUid)
{
  const std::filesystem::path dataFilePath = ProduceDataFilePath(
    _rewardDataPath, std::format("{}", claimUid));
  // ★УДАЛЕНИЕ ОТ ДЕСКРИПТОРА КАТАЛОГА, И ЕГО ВЕРДИКТ ЧИТАЕТСЯ (итерации 6 и 7).
  // `remove` проходит промежуточные ссылки насквозь — это закрыла итерация 6;
  // но она же ПОТЕРЯЛА отказ: прежний бросающий `remove` доносил `EACCES`/
  // `EROFS`/`EIO` до `DataDirector`, тот возвращал `false`, и `DataStorage`
  // ОСТАВЛЯЛ запись в кэше. Замена на код возврата, который никто не читал,
  // объявляла удаление состоявшимся: файл лежал на диске, имя числилось
  // свободным, а запись воскресала после перезапуска.
  RemoveDataFileOrThrow(dataFilePath, "Reward file");
}
