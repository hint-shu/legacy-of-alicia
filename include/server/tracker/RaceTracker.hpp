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

#ifndef RACETRACKER_HPP
#define RACETRACKER_HPP

#include "server/tracker/Tracker.hpp"

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/network/command/proto/CommonStructureDefinitions.hpp>
#include <libserver/registry/ItemRegistry.hpp>

#include <array>
#include <chrono>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace server::tracker
{

//! Invalid course time represents a did not finish state in the client scoreboard.
constexpr uint32_t InvalidCourseTime = std::numeric_limits<uint32_t>::max();

//! LOA-fix (A3, round3): минимально правдоподобное время прохождения трассы, мс.
//! ⚠️ ЧТО ЭТИМ ГЕЙТИТСЯ (N1, round7 — актуально после отката раунда 6):
//! ТОЛЬКО КВЕСТОВЫЙ ПРОГРЕСС — сюжетные счётчики заездов (B1), призовые места и
//! диспатч квестов в RaceInstance::Stop. ВЫПЛАТУ (2500 морковок / 420 опыта)
//! этот порог НЕ трогает: выплатной античит заездов (A1 / A3-множитель / B2 /
//! C1) откачен раундом 6 как нерабочий, награда считается по АПСТРИМНОМУ
//! правилу — безусловно. Не читать этот комментарий как «короткий заезд не
//! оплачивается деньгами»: оплачивается.
//! ПОЧЕМУ КОНСТАНТА, А НЕ ДАННЫЕ КАРТЫ: в courses.yaml длины трасс нет вообще,
//! а timeLimit у всех 52 боевых карт одинаковый (300 с; 30 с только у ranch_00 /
//! ranch_w / town_test01, которые заездами не являются) — вывести честный
//! минимум «по карте» не из чего. 30 с выбраны с большим запасом: реальный круг
//! в игре идёт полторы-три минуты. ТЮНИНГ: если в логе появятся отказы честным
//! игрокам («course time … is below the plausible minimum»), порог опустить.
constexpr uint32_t MinPlausibleCourseTime = 30000;

//! Максимум правдоподобного прироста класс-опыта за ОДИН заезд (#93). Клиент
//! шлёт gainedClassProgress сам; без капа модифицированный клиент чеканит
//! классы одним пакетом. 10000 = 2.92× наблюдаемого по pcap максимума (3429) и
//! 1.5 класса (levelUpPoints.base 6650). ТЮНИНГ: если в логе пойдут отказы
//! честным («exceeding the plausible per-race maximum»), порог поднять.
constexpr uint32_t MaxPlausibleClassProgress = 10000;

//! LOA-fix (R24, #14 фаза 1): потолок правдоподобной скорости за заезд. member4 —
//! КЛИЕНТСКИЙ и неверифицированный; без капа модклиент чеканит ВЕЧНЫЙ рекорд
//! mountInfo.topSpeed (running max, откат только правкой данных при остановленном
//! сервере). 300 км/ч = 2.94x наблюдённого по pcap максимума (101.91) — та же
//! пропорция, что в MaxPlausibleClassProgress. Тюнинг: если честные упрутся —
//! в логе пойдёт warn, поднять.
constexpr float MaxPlausibleSpeedKph = 300.0f;

//! LOA-fix (R24, #14 фаза 1): жёсткий потолок ОДНОГО шага позиции, метры. Основной
//! фильтр — динамический бюджет MaxPlausibleSpeedKph*dt; эта константа страхует
//! случай, когда залип сам сервер и dt выдал огромный бюджет. Наблюдённый макс шага
//! в pcap — 10.68 м при каденции 3.83 Гц.
constexpr float MaxPlausiblePositionDeltaMetres = 200.0f;

//! LOA-fix (R76, backlog #30 этап 1): ПОРОГИ ЖУРНАЛА ТРАССЫ. Все четыре — ТОЛЬКО
//! для WARN в журнале; ни один не гейтит выплату, квесты или рассылку.
//! ★ЧИСЛА ВЗЯТЫ ИЗ ИЗМЕРЕНИЯ, А НЕ ИЗ ГОЛОВЫ: раунд 0 разобрал 86 честных заездов
//! живой игры на 8 картах (specs/R76-progress-table.md).

//! Минимум взятых 10 %-порогов, ниже которого финиш считается «жидким».
//! Честный минимум по 86 заездам — 8/10 (карта 7 ri_fore01 финиширует при
//! progress 0.811). 6 оставляет два сплита запаса. ТЮНИНГ: если в логе пойдут
//! «thin ride» у честных — снижать; НИКОГДА не повышать без нового замера.
constexpr uint8_t MinPlausibleSplits = 6;

//! ВЕРХНЯЯ граница среднего интервала между пакетами позиции, при которой заезд
//! ещё считается реально проеханным, мс. Имя читать как «максимальный
//! правдоподобный СРЕДНИЙ интервал»: это анти-«заезд в три пакета», а НЕ
//! анти-флуд. Честный каденс 3.77-4.00 Гц (интервал 250-265 мс); 500 мс =
//! 1.89x запаса. Нужен В ПАРЕ со сплитами: после паузы бюджет храповика
//! вырастает и ОДИН пакет штампует все 10 сплитов (наблюдено на 4 заездах с
//! дырой захвата 133 с) — «сплитов 10» само по себе не доказывает ничего.
constexpr uint32_t MaxPlausibleMeanPosIntervalMs = 500;

//! Величина ОДНОГО отброшенного шага позиции, начиная с которой это телепорт, м.
//! ★НЕ СЧЁТЧИК: 24 из 86 честных заездов имеют positionJumps > 0 (до 10 за заезд),
//! потому что склеенные в один приход пакеты дают dt = 0 и бюджет 0. По ВЕЛИЧИНЕ
//! честный максимум одного отброшенного шага — 63.25 м; 150 м = 2.37x запаса.
constexpr float TeleportStepMetres = 150.0f;

//! Потолок СЫРОГО клиентского progress, выше которого объявление абсурдно.
//! ★ПОЧЕМУ НЕ ПЕР-КАРТОВЫЙ ПОРОГ. Финишный «часовой» — пер-картовая константа
//! (1.2 / 1.5 / 2.0 на измеренных картах), но измерено 8 map-блоков из 55:
//! пер-картовая таблица дала бы WARN каждому честному финишу на 47 неизмеренных
//! картах. 4.0 = 2x максимального наблюдённого часового. Фактический максимум
//! пишется в аудит-строку ВСЕГДА — из этих строк раунд 2 построит таблицу по
//! живому трафику.
constexpr float MaxDeclaredProgressCeiling = 4.0f;

//! LOA-fix (R75, #14 Ф2): ДОПУСК СОПОСТАВЛЕНИЯ отметки «планирование» с отрезком
//! полёта. Клиент шлёт 0xe7 (AcCmdCRHurdleClearResult) и позицию (0x103f) РАЗНЫМИ
//! пакетами, и порядок их прихода нам НЕ известен: отметка может лечь на пакет
//! раньше первого воздушного кадра, внутрь полёта или на пакет позже приземления.
//! Каденция позиции в проде — 3.83 Гц (0.26 с между пакетами, замер R24 по pcap),
//! поэтому 500 мс = два таких интервала в обе стороны. ТЮНИНГ: сузить, если
//! окажется, что метка цепляет соседний барьер; расширять НЕЛЬЗЯ — поле
//! longestGlideDistance это ВЕЧНЫЙ максимум, откат только правкой данных при
//! остановленном сервере.
constexpr std::chrono::milliseconds GlideMarkTolerance{500};

//! LOA-fix (R75, #14 Ф2): ОКНО УСТАРЕВАНИЯ ЦЕПОЧКИ РЫВКОВ. Серверной длительности
//! буста НЕ СУЩЕСТВУЕТ (в коде нет ни таймера буста, ни его конца), поэтому
//! «подряд» приходится определять самим. Определяем ДВУМЯ признаками, и оба умеют
//! только ОБОРВАТЬ цепочку, ни один не умеет её удлинить: (1) клиентский флаг
//! AcCmdCRRequestSpur::comboBreak — врать им в свою пользу нельзя, «я сломал
//! комбо» только уменьшает число; (2) вот это окно — два платных рывка дальше
//! друг от друга, чем окно, цепочкой не считаются. 30 с выбраны с запасом: рывок
//! стоит 40 000 звёздных очков при потолке шкалы 120 000
//! (resources/config/game/courses.yaml, gameModeInfo type 1), то есть честная
//! серия идёт секундами, а не десятками секунд; круг же длится 90-180 с, и без
//! окна рывки из разных концов трассы слились бы в одну фальшивую серию.
constexpr std::chrono::seconds BoostComboWindow{30};

//! LOA-fix (R75, #14 Ф2): ПОТОЛОК ПРАВДОПОДОБИЯ ОДНОГО ОТРЕЗКА ПЛАНИРОВАНИЯ, метры.
//!
//! ★ЗАЧЕМ. «В воздухе» — это КЛИЕНТСКОЕ булево `AcCmdUserRaceUpdatePos::member5`
//! («1 = In the air»). Клиенту достаточно один раз прислать 0xe7 типа 2 и держать
//! member5 = 1 до конца заезда, чтобы отрезок копил ВЕСЬ оставшийся путь. Соседние
//! поля защищены величиной (topSpeed клампится MaxPlausibleSpeedKph, totalDistance
//! — насыщающийся одометр), а longestGlideDistance — РЕКОРД, монотонно вверх, и
//! снять его можно только правкой файлов при ОСТАНОВЛЕННОМ сервере.
//!
//! ★ОТКУДА ЧИСЛО. Прямой длины карт в конфиге нет; выводим из наблюдаемого:
//! настоящий захваченный круг R24 — 3447.184 м суммарного пути. 1000 м одним
//! НЕПРЕРЫВНЫМ полётом — это 29% целого круга, то есть заведомо выше любого
//! честного планирования; при потолке скорости 300 км/ч (83.3 м/с) это ещё и
//! 12 секунд непрерывного воздуха. ТЮНИНГ: опускать после живой проверки глазами
//! МОЖНО; поднимать НЕЛЬЗЯ — поле вечное, и поднятый потолок задним числом
//! узаконит уже записанную ложь.
constexpr float MaxPlausibleGlideMetres = 1000.0f;

//! LOA-fix (R75, #14 Ф2): ПОТОЛОК ПРАВДОПОДОБИЯ ЦЕПОЧКИ ПЛАТНЫХ РЫВКОВ.
//!
//! ★ЗАЧЕМ. Симметрично планированию, и по той же причине: шкала звёздных очков
//! НЕ является серверной ценой. `HandleStarPointGet` начисляет ровно то, что
//! КЛИЕНТ объявил в `gainedStarPoints`, клампя лишь потолком шкалы. Значит
//! «платный рывок» оплачивается величиной, которую платящий сам себе и назначил,
//! и без потолка цепочка так же неограниченно накручивается, как и глайд. Дыру в
//! самом StarPointGet раунд НЕ чинит — он ставит потолок там, где величина
//! становится ВЕЧНОЙ.
//!
//! ★ОТКУДА ЧИСЛО. Честная экономика: рывок стоит 40 000, барьер даёт не больше
//! 12 000 + 5×1000 комбо-бонуса = 17 000 (courses.yaml, type 1), то есть на один
//! рывок нужно минимум 3 барьера. Круг в 300 с с барьером каждые ~2 с даёт ≤150
//! барьеров, то есть ≤50 честных рывков за ВЕСЬ заезд — а цепочка это ещё и
//! непрерывная их часть. 50 — потолок с запасом в разы.
constexpr uint32_t MaxPlausibleBoostChain = 50;

//! LOA-fix (R70, backlog #58): ЗАЯВЛЕННАЯ доля трассы, ниже которой гонщик не
//! считается участником. Переехала сюда из анонимного namespace
//! `RaceInstance.cpp` (итерация 3): ту же величину теперь спрашивает и
//! `HandleUserRaceFinal`, а две копии одного порога разъезжаются молча.
//! ★ЧТО ЭТО ЗА ВЕЛИЧИНА: `Racer::trustedProgress` — серверная копия клиентского
//! прогресса (R-revenge, #13), которая растёт не быстрее правдоподобного темпа и
//! никогда не убывает. Она ОГРАНИЧЕНА ВРЕМЕНЕМ, а не путём, поэтому САМА ПО СЕБЕ
//! участия НЕ доказывает (см. `Racer::HasProvenTraversal`) — это лишь одна из
//! двух улик.
constexpr float MinMeaningfulRaceProgress = 0.1f;

//! LOA-fix (R70 итерация 3, backlog #58): СКОЛЬКО МЕТРОВ ПУТИ СЕРВЕР ОБЯЗАН
//! НАМЕРЯТЬ САМ, чтобы признать гонщика участником заезда.
//!
//! ★ПОЧЕМУ ИМЕННО ПУТЬ. `distanceMetres` — единственная величина заезда,
//! которую считает СЕРВЕР и которую нельзя объявить: она копится в
//! `HandleRaceUserPos` из РАЗНИЦЫ ПОЗИЦИЙ, только после зелёного света, только
//! в состоянии `Racing` и только для шагов, влезших в бюджет правдоподобия
//! `min(MaxPlausiblePositionDeltaMetres, MaxPlausibleSpeedKph * dt)` при dt,
//! зажатом двумя секундами. Отсюда потолок ОДНОГО принятого шага — 166.7 м
//! (300 км/ч × 2 с), то есть двести метров нельзя купить ни одним пакетом:
//! нужны минимум два принятых шага и, даже на потолке скорости, не меньше
//! 2.4 секунды непрерывного правдоподобного движения. Первый пакет позиции
//! вообще только СЕЕТ точку отсчёта и пути не приносит.
//! ★ПОЧЕМУ 200. Мерить долей трассы не из чего: длины трассы у сервера нет
//! (в `courses.yaml` её не существует). Известен один замер — centerline
//! `ri_land01` = 1801 м (разбор захвата, профиль тестера), то есть 200 м это
//! примерно тот же «десяток процентов трассы», что и `MinMeaningfulRaceProgress`.
//! Честному гонщику порог не мешает: по тому же захвату клиент шлёт позицию с
//! каденцией 3.83 Гц шагами до 10.68 м, и двести метров набираются за первые
//! секунды заезда — задолго до финиша, до схода и до подведения итогов.
//! ★ЧЕГО ПОРОГ НЕ ДЕЛАЕТ (прямо, чтобы не читалось шире): отличить скрипт от
//! человека сервер не может в принципе. Он поднимает цену альта с «войти и
//! молчать» и «прислать два пакета» до «слать правдоподобную телеметрию
//! движения столько же, сколько едет игрок». Полностью закрыть сговор живых
//! аккаунтов нельзя ничем, кроме отказа от записей с `numPlayer`.
constexpr double MinMeaningfulTraversalMetres = 200.0;

constexpr std::chrono::milliseconds EventThrottleDuration{250};

//! LOA-fix (R-revenge, #13): ПАРАМЕТРЫ АНТИ-ФОРЖА БОНУСА «НЕПЛОХАЯ МЕСТЬ».
//! Месть определяется ТОЛЬКО сервером. Клиентская декларация цели (опкод 0x206
//! AcCmdCRRevengeAssign) в процесс НЕ входит: структуры и хендлера для неё нет,
//! и реализовывать её НЕЛЬЗЯ — это была бы дыра «объяви себе месть сам».
//! Гистерезис обгона: доля трассы, на которую надо ОТОРВАТЬСЯ, чтобы обгон
//! засчитался. Дребезг «ноздря в ноздрю» мести не даёт.
constexpr float RevengeOvertakeHysteresis = 0.01f;

//! Выдержка: столько надо НЕПРЕРЫВНО удерживать отрыв, чтобы позиция считалась
//! устоявшейся. Убивает пинг-понг на финишной прямой и обмен «мгновенными»
//! обгонами двух сговорившихся аккаунтов.
constexpr std::chrono::milliseconds RevengeDwellDuration{3000};

//! Максимальный возраст последнего пакета позиции СОПЕРНИКА, при котором его
//! прогресс считается живым. Отвалившийся/залагавший соперник замораживает свой
//! trustedProgress — «обгон трупа» местью не считается.
constexpr std::chrono::milliseconds RevengeRivalFreshness{3000};

//! Потолок числа зачтённых соперников (третий порог клиентской шкалы). Больше
//! не платим: тиры 500/750/1000 морковок и 100/150/200 класс-опыта.
constexpr uint32_t RevengeMaxCredits = 3;

//! A race tracker.
class RaceTracker
{
public:
  //! A per-racer event item (e.g. egg) visible only to one racer.
  struct EventItem
  {
    Oid oid{};
    uint32_t itemType{};
    protocol::Vector3 position{};
  };

  //! LOA-fix (R68, backlog #5/#99): КВЕСТОВЫЙ ПРЕДМЕТ гонщика.
  //!
  //! ★ОТДЕЛЬНАЯ СТРУКТУРА, А НЕ `EventItem`, и это не вкусовщина. У яичного
  //! вектора стоит ЖЁСТКИЙ КАП `MaxEventItemsPerRacer = 1` (гард #125b в
  //! `HandleGameCreateClientItem`), а квестовых предметов гонщику
  //! раскладывается до 32 за заезд — переиспользование либо сломало бы гард
  //! яиц, либо обрезало бы раскладку до одного предмета.
  struct QuestItem
  {
    //! Object id предмета. Выдаётся из ТОГО ЖЕ счётчика `_nextItemDeckOid`,
    //! что у обычных деков и яиц: у клиента все предметы заезда живут в одном
    //! пространстве идентификаторов.
    Oid oid{};
    //! Внешний вид (клиентская таблица `DeckItemParam`, напр. 1003 = осколок
    //! кристалла). Уходит клиенту как `AcCmdRCCreateItem::itemType`.
    uint32_t itemType{};
    //! `QTemID` предмета — то же число, что в `Quest::functionValue`.
    //! Используется как СВЕРКА при засчитывании, а не как идентификатор цели.
    uint32_t questItemId{};
    //! ★TID КВЕСТА, РАДИ КОТОРОГО ЭТОТ ПРЕДМЕТ ПОЛОЖЕН, — и это главное поле.
    //! Одного `questItemId` НЕ ХВАТАЕТ: пять сюжетных квестов делят QTemID 3
    //! (12016/14014/14019/14020/14021), три — QTemID 6 (12015/14024/14028), два
    //! — QTemID 5 (12013/12014). Если засчитывать по предмету, один подобранный
    //! осколок двигал бы ВСЕ активные квесты своего класса разом — «печать»
    //! прогресса из одного объекта. Предмет кладётся ПЕР-КВЕСТОВО и
    //! засчитывается ровно в тот квест, ради которого лежал.
    uint32_t questTid{};
    //! Мировая позиция предмета (уже со смещением карты).
    protocol::Vector3 position{};
  };

  //! A racer.
  struct Racer
  {
    enum class State
    {
      Disconnected,
      Loading,
      Racing,
      Finishing,
    };

    //! LOA-fix (R70, backlog #58): ЧЕМ КОНЧИЛСЯ ЗАЕЗД ДЛЯ ЭТОГО ГОНЩИКА.
    //!
    //! ★ЗАЧЕМ ОТДЕЛЬНОЕ ПОЛЕ, ЕСЛИ ЕСТЬ `courseTime`. `courseTime ==
    //! InvalidCourseTime` означает СРАЗУ ТРИ РАЗНЫХ СОБЫТИЯ, и достижения не
    //! имеют права их путать:
    //!   (1) гонщик объявил СХОД (`raceTrackProgress > 0` в
    //!       `AcCmdUserRaceFinal`) И СЕРВЕР ЭТОТ СХОД ПОДТВЕРДИЛ — это и есть
    //!       «сход» оригинала (`ach_conditions.lua`, `Retire`:
    //!       `TimeRecord == 0`). Подтверждение обязательно: `raceTrackProgress`
    //!       — клиентское поле, и без серверной проверки «заезд к этому моменту
    //!       шёл не меньше `MinPlausibleCourseTime`» мгновенный DNF чеканил бы
    //!       10036 и печатал напарнику 10008 (ревью R70, итерация 2);
    //!   (2) гонщик объявил ФИНИШ, а его ОТВЕРГ АНТИЧИТ — финиш до зелёного
    //!       света либо быстрее `MinPlausibleCourseTime`
    //!       (`RaceNetworkHandler::HandleUserRaceFinal`). Приравнять это к
    //!       сходу значило бы ПЛАТИТЬ ЗА ПОПЫТКУ ОБМАНА: соло-читер, четырежды
    //!       «мгновенно финишировавший», добирал бы все четыре тира «Обидный
    //!       сход» (10036, 10 очков);
    //!   (3) гонщик не присылал ничего вовсе (вышел, завис, простоял заезд) —
    //!       ни финиша, ни схода не было.
    //! Поле отвечает на вопрос ПОЛОЖИТЕЛЬНО («сход доказан»), а не отрицанием
    //! («времени нет, значит сход»): доказывать надо наличие события, а не его
    //! отсутствие.
    //!
    //! Живёт ровно один заезд — там же, где `finishCounted`: `Tracker::Clear()`
    //! сносит гонщика целиком, `AddRacer()` создаёт его заново с `None`.
    enum class FinishOutcome
    {
      //! Ни финиша, ни схода гонщик не заявлял.
      None,
      //! Заявил финиш, время принято сервером.
      Finished,
      //! Заявил СХОД, и сервер его ПОДТВЕРДИЛ (заезд шёл не меньше
      //! `MinPlausibleCourseTime`). Единственное значение, которое достижения
      //! считают сходом.
      Retired,
      //! Заявленный исход СЕРВЕР ОТВЕРГ: финиш до зелёного света либо быстрее
      //! `MinPlausibleCourseTime`, а равно и «сход» с заезда, который столько
      //! ещё не шёл. Ни финишем, ни сходом НЕ является.
      Rejected,
    };

    using Team = protocol::TeamColor;

    struct ItemInstance
    {
      std::chrono::steady_clock::time_point expiryTimePoint;
    };

    Oid oid{InvalidEntityOid};
    State state{State::Disconnected};
    Team team{Team::Solo};
    //! The racer's position in the world, as a vector.
    protocol::Vector3 worldPosition{};
    uint32_t starPointValue{};
    uint32_t jumpComboValue{};
    uint32_t courseTime{InvalidCourseTime};
    //! LOA-fix (R7 BLOCK-1, round7): латч «финиш этого гонщика уже засчитан в
    //! КВЕСТОВЫЕ счётчики заездов». Состояние-НЕЗАВИСИМЫЙ: racer.state для
    //! дедупа не годится — HandleLoadingComplete безусловно возвращает Racing
    //! из ЛЮБОГО состояния (апстримное поведение; гард C1 откачен раундом 6),
    //! поэтому чередование LoadingComplete → RaceFinal сбрасывало прежнюю
    //! проверку `state == Finishing` и накручивало счётчики.
    //! Живёт ровно один заезд: HandleStartRace зовёт Tracker::Clear(), после
    //! чего AddRacer() создаёт Racer заново (значение по умолчанию false).
    //! Денег НЕ касается: выплата в RaceInstance::Stop это поле не читает.
    bool finishCounted{false};
    //! LOA-fix (R70, backlog #58): чем кончился заезд для гонщика. Пишется ТАМ
    //! ЖЕ, где `courseTime`, и под тем же латчем `finishCounted` — первый пакет
    //! финиша/схода фиксирует исход, повторные его не переписывают.
    FinishOutcome finishOutcome{FinishOutcome::None};
    std::optional<uint32_t> magicItem{};
    //! The racer's progress on the race track.
    //! Normalised by the client to: 0.0f <= x <= 1.0f
    float raceProgress{};

    //! LOA-fix (R-revenge, #13): ДОВЕРЕННЫЙ прогресс — серверная копия
    //! raceProgress, которая (а) растёт не быстрее правдоподобного темпа
    //! (вся трасса за MinPlausibleCourseTime) и (б) НИКОГДА не убывает.
    //! ПОЧЕМУ ОТДЕЛЬНОЕ ПОЛЕ: по raceProgress ранжирует MagicSystem.cpp:94
    //! (выдача магических предметов по месту) — любая правка семантики там
    //! была бы регрессией магии. Здесь нулевой риск.
    //! ПОЧЕМУ МОНОТОННО (★самая тонкая дыра): если позволить прогрессу падать,
    //! модклиент занижает свой progress → «его обогнали» → возвращает честный →
    //! «месть» без единого реального обгона. Храповик это закрывает.
    //! Живёт РОВНО один заезд (сброс там же, где finishCounted).
    float trustedProgress{};
    //! Момент последнего движения trustedProgress. Служит двум целям: бюджет
    //! темпа (свой) и проверка свежести (чужой). max() = ещё не стартовал.
    std::chrono::steady_clock::time_point trustedProgressTimePoint{
      std::chrono::steady_clock::time_point::max()};

    //! Состояние «мести» относительно ОДНОГО соперника из чужой команды.
    enum class RevengeState
    {
      //! Нас ещё не обгоняли (или обгон не выдержан).
      Idle,
      //! Соперник устойчиво впереди — «обидчик» зафиксирован.
      Passed,
      //! Мы устойчиво вернулись вперёд — месть зачтена. Терминальное.
      Revenged,
    };

    struct RevengeRow
    {
      RevengeState state{RevengeState::Idle};
      //! Момент, с которого текущее ожидаемое отношение (соперник впереди /
      //! мы впереди) держится непрерывно. max() = отношение сейчас не выполнено.
      std::chrono::steady_clock::time_point since{
        std::chrono::steady_clock::time_point::max()};
    };

    //! Строки мести, ключ — characterUid соперника. Размер ограничен числом
    //! гонщиков чужой команды (<= 7). Живёт ровно один заезд.
    std::map<data::Uid, RevengeRow> revengeRows;
    //! Число ТЕРМИНАЛЬНЫХ мстей за заезд — источник тира выплаты в
    //! RaceInstance::Stop(). Клампится RevengeMaxCredits.
    uint32_t revengeCredits{};

    //! LOA-fix (R24, #14 фаза 1): пер-заездная телеметрия для mountInfo лошади.
    //! Живёт РОВНО один заезд: HandleStartRace зовёт Tracker::Clear() → AddRacer()
    //! создаёт Racer заново с дефолтами (+ явный сброс там же, где finishCounted).
    //! ПОТОКИ: под _raceInstancesMutex (как весь трекер), НЕ атомарные. Трогать
    //! только из Handle*-хендлера под локом либо из RaceInstance::Stop().
    //! Максимум клиентской скорости (member4) за заезд, км/ч.
    float topSpeedKph{};
    //! Пройденный путь за заезд, метры (1 мир-юнит ≈ 1 метр).
    double distanceMetres{};
    //! Первый пакет позиции только СЕЕТ worldPosition (иначе первая дельта от
    //! {0,0,0} принесёт ~8000 ложных «метров»).
    bool hasPositionSample{false};
    //! Момент прошлого пакета позиции — для бюджета правдоподобного шага.
    std::chrono::steady_clock::time_point lastPositionTimePoint{
      std::chrono::steady_clock::time_point::max()};

    //! LOA-fix (R76, backlog #30 этап 1): ПЕР-ЗАЕЗДНЫЙ ЖУРНАЛ ТРАССЫ.
    //! ★ЧИСТОЕ НАБЛЮДЕНИЕ. Ни одно поле ниже не читает ни выплата
    //! (RaceInstance::Stop, начисление безусловно), ни квесты, ни рассылка, ни
    //! MagicSystem. Раунд добавляет ЗНАНИЕ, а не правило: выплатной гард по
    //! прогрессу откатывали раундом 6, и повторять его до окна наблюдения нельзя.
    //! ПОТОКИ: те же, что у R24 — под уже взятым _raceInstancesMutex, не атомарные.
    //! Живут РОВНО один заезд: сброс стоит там же, где finishCounted/topSpeedKph.

    //! Число 10 %-порогов трассы.
    static constexpr size_t ProgressSplitCount = 10;
    //! «Порог не взят». Отличается от честного «взят на 0-й миллисекунде».
    static constexpr uint32_t InvalidSplitMs = 0xFFFFFFFFu;

    //! Момент (мс от ЗЕЛЁНОГО СВЕТА), когда trustedProgress ВПЕРВЫЕ пересёк
    //! 0.1*(k+1). Значимы только элементы [0, splitsReached) — остальные обязаны
    //! нести InvalidSplitMs (заполняется сбросом в HandleStartRace).
    //! ★ПО ДОВЕРЕННОМУ, А НЕ ПО СЫРОМУ: сырой клиентский progress прыгает до
    //! 0.249/с на честных данных и до чего угодно на модклиенте; храповик R13
    //! уже несёт бюджет темпа и монотонность, и сплиты обязаны наследовать
    //! именно его свойства.
    std::array<uint32_t, ProgressSplitCount> progressSplits{};
    //! Сколько порогов взято. Монотонно растёт, максимум ProgressSplitCount.
    uint8_t splitsReached{};

    //! Принятых пакетов позиции ПОСЛЕ зелёного света. Знаменатель проверки
    //! «заезд в три пакета»: courseTime/posSampleCount — средний интервал.
    uint32_t posSampleCount{};
    //! Сколько раз шаг позиции не влез в бюджет R24 (строка `if (step <= budget)`).
    //! ★НЕ СИГНАЛ САМ ПО СЕБЕ: у честных до 10 за заезд (склейка пакетов, dt = 0).
    uint32_t positionJumps{};
    //! Сумма отброшенного, м — масштаб.
    double discardedMetres{};
    //! Максимум ОДНОГО отброшенного шага, м — вот это и есть сигнал телепорта.
    float maxDiscardedStepMetres{};
    //! Сколько раз бюджет темпа обрезал заявленный клиентом прогресс.
    //! ★ТОЖЕ НЕ СИГНАЛ: у 67 из 86 честных заездов > 0, до 18 за заезд.
    uint32_t progressClipped{};
    //! Максимум СЫРОГО command.progress за заезд, ДО всякой санитизации. Ловит
    //! объявленные 1.5 / 2.0 (честные финишные часовые) и 999 / 1e38 (модклиент).
    //! NaN сюда не попадает: сравнение `>` с NaN ложно.
    //! ★ОКНО ШИРЕ ОСТАЛЬНЫХ ПОЛЕЙ: пишется последней строкой функции, ВНЕ гейта
    //! зелёного света и вне `state == Racing` (см. врезку C). То есть declared
    //! видит и предстартовые, и постфинишные объявления, а splits/samples/jumps/
    //! clipped — нет. Для наблюдательного раунда это плюс (шире охват); раунд 2
    //! обязан знать это, иначе построит таблицу по несопоставимым величинам.
    float maxDeclaredProgress{};

    //! LOA-fix (R76-fix-1, backlog #30 этап 1, находка Codex 1 WARN-3):
    //! ПЛОТНОСТЬ ПАКЕТОВ ПОЗИЦИИ — «на этот заезд пакетов вообще хватило?».
    //!
    //! ★ЗАЧЕМ ОТДЕЛЬНАЯ ФУНКЦИЯ, А НЕ ВЫРАЖЕНИЕ ПО МЕСТУ. Условие живёт внутри
    //! WARN «жидкий заезд» в `RaceInstance::LogRaceAudit()`, то есть в цели,
    //! которая в тестовый бинарь не линкуется. Пока оно стояло выражением,
    //! юнит-тест мог лишь ПОВТОРИТЬ его у себя — и повторение оставалось
    //! зелёным даже со снятым `static_cast<uint64_t>` в проде (проверено
    //! ревью). Функция в заголовке — единственная форма, при которой тест
    //! проверяет ТОТ ЖЕ КОД, что исполняется в бою.
    //!
    //! ★ПОЧЕМУ 64 БИТА. `posSampleCount` — `uint32_t`, множитель 500. В 32
    //! битах произведение переполняется при ~8.6 млн пакетов, и заезд, в
    //! котором пакетов пришло СЛИШКОМ МНОГО, получил бы маленькое произведение,
    //! то есть флуд читался бы как «никто не ехал». Каст обязателен, и его
    //! снятие обязано валить тест.
    //!
    //! @param courseTimeMs Длительность заезда по серверному замеру, мс.
    //! @returns true, если средний интервал между пакетами не хуже порога.
    [[nodiscard]] constexpr bool HasPlausiblePacketDensity(
      const uint32_t courseTimeMs) const
    {
      return static_cast<uint64_t>(posSampleCount)
        * MaxPlausibleMeanPosIntervalMs >= courseTimeMs;
    }

    //! LOA-fix (R70 итерация 3, backlog #58): ЕХАЛ ЛИ ЭТОТ ГОНЩИК — по улике,
    //! которую собрал САМ СЕРВЕР.
    //!
    //! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ПРЕДИКАТ И ПОЧЕМУ ОН ОДИН НА ВЕСЬ ЗАЕЗД. Ревью
    //! итерации 3 назвало три разных дефекта достижений — «прогресс покупается
    //! двумя пакетами», «исход доказывается часами», «обрыв стирает участие», —
    //! и у всех трёх ОДНА причина: участие доказывалось чем угодно, кроме
    //! доказательства езды. Один предикат, а не три гарда по местам: гарды
    //! разъезжаются (итерация 2 это уже показала), инвариант — нет.
    //!
    //! ★ДВЕ УЛИКИ, ОБЕ ОБЯЗАТЕЛЬНЫ И ОБЕ СЕРВЕРНЫЕ ПО РАЗНОМУ.
    //!  (1) `distanceMetres >= MinMeaningfulTraversalMetres` — путь, который
    //!      сервер СЧИТАЛ САМ из разниц позиций, отбрасывая шаги вне бюджета
    //!      правдоподобия. Это единственная улика, которую нельзя ОБЪЯВИТЬ:
    //!      её цена — время под потолком скорости.
    //!  (2) `trustedProgress >= MinMeaningfulRaceProgress` — заявленный прогресс,
    //!      зажатый серверным темпом и храповиком. Клиент шлёт его в тех же
    //!      пакетах, поэтому честному игроку лег не стоит ничего, а альту,
    //!      который двигает точку, но «не участвует» в гонке по своим же
    //!      данным, стоит ещё трёх секунд связной лжи.
    //!
    //! ★СОСТОЯНИЕ СОЕДИНЕНИЯ ЗДЕСЬ НЕ СПРАШИВАЕТСЯ, И ЭТО РЕШЕНИЕ, А НЕ
    //! ЗАБЫВЧИВОСТЬ. `state` отвечает, где игрок СЕЙЧАС; участие — это история
    //! заезда. Пока состав считался по соединению, честный финишёр, закрывший
    //! игру, исчезал из заезда задним числом, а проигравший получал выключатель
    //! записей с `numPlayer`: вышел — и заезд «стал» меньше. Соединение решает
    //! ровно один вопрос — куда доставить уведомление.
    //!
    //! ★ЖИВЁТ РОВНО ОДИН ЗАЕЗД: обе величины сбрасываются там же, где
    //! `finishCounted` (`HandleStartRace` + `Tracker::Clear`/`AddRacer`).
    //!
    //! ★ЧЕГО ЭТА УЛИКА НЕ ДОКАЗЫВАЕТ — записанный остаточный риск (решение
    //! лида R70 ит.4) и `TODO(backlog #30 этап 1, #32)` лежат в ОПРЕДЕЛЕНИИ
    //! (`RaceTracker.cpp`), чтобы жить рядом с кодом, а не рядом с сигнатурой.
    //! Коротко: сумма клиентских дельт не становится НАБЛЮДЕНИЕМ оттого, что
    //! складывает её сервер, — модифицированный клиент участие всё ещё купит.
    [[nodiscard]] bool HasProvenTraversal() const;

    //! A set of tracked items in racer's proximity.
    std::unordered_set<Oid> trackedDecks;
    //! A deck cooldown time point tracker.
    std::unordered_map<Oid, std::chrono::steady_clock::time_point> deckCooldown;

    //! Per-racer event items (e.g. eggs) visible only to this racer.
    std::vector<EventItem> eventItems;

    //! LOA-fix (R68, backlog #5/#99): пер-гонщиковые КВЕСТОВЫЕ предметы.
    //! Живут ровно один заезд: раскладываются в `HandleStartRace` сразу после
    //! `AddRacer` (то есть уже после `Tracker::Clear()`), а `Clear()` сносит
    //! гонщиков целиком вместе с этим вектором.
    std::vector<QuestItem> questItems;

    //! Active skill effects indexed by skillEffectId (0-23).
    static constexpr size_t EffectCount = 24;
    std::array<bool, EffectCount> effects{};
    //! Per-effect generation counter, incremented on each apply, used to invalidate stale removal timers.
    std::array<uint32_t, EffectCount> effectGenerations{};

    //! Rank of the currently active removeMagic attack (0 = none active).
    uint32_t attackRank{};
    std::chrono::steady_clock::time_point dragonReceivedAt{};

    //! Anchor for time-based magic gauge regen. Default-constructed = uninitialized,
    //! lazily set to raceStartTimePoint on the first regen tick.
    std::chrono::steady_clock::time_point lastGaugeUpdateTimePoint{
      std::chrono::steady_clock::time_point::max()};

    //! Snapshot of the racer's mount stats taken at race start, used by per-tick
    //! magic-mode calculations to avoid a DataDirector lookup on every pos-update.
    struct MountStatsSnapshot
    {
      uint32_t agility{};
      uint32_t ambition{};
      uint32_t rush{};
      uint32_t endurance{};
      uint32_t courage{};
    };
    MountStatsSnapshot mountStats{};
    registry::SetEquipEffect activeSetEffect{registry::SetEquipEffect::None};

    struct MagicTargetInfo
    {
      uint16_t casterOid;
      uint16_t effectInstanceId;
    };
    std::optional<MagicTargetInfo> pendingMagicTarget{};

    // === LOA-fix (R75, #14 Ф2): ПЛАНИРОВАНИЕ И ЦЕПОЧКА РЫВКОВ ==============
    //! ★ЭТИ ПОЛЯ СТОЯТ В КОНЦЕ СТРУКТУРЫ НАМЕРЕННО. Racer читают функции,
    //! которые раунд не правит (HandleStarPointGet — racer.effects), и одна из
    //! них служит НЕРАСТУЩИМ КОНТРОЛЕМ лесенки. Вставка в середину сдвинула бы
    //! смещения полей после точки вставки и могла бы изменить размер их кода —
    //! контроль «вырос» бы, не изменившись ни на строку. Конец структуры это
    //! исключает по построению. Не переносить выше «ради группировки».
    //!
    //! Живут РОВНО один заезд и обнуляются там же, где телеметрия R24 и латч
    //! finishCounted (RaceNetworkHandler::HandleStartRace) — по той же причине:
    //! трекер переиспользуется, и без явного сброса чужой полёт уехал бы в лошадь.
    //! ПОТОКИ: под _raceInstancesMutex, как весь трекер.

    //! Был ли гонщик в воздухе на ПРОШЛОМ пакете позиции (member5 == 1).
    bool previousAirborne{false};
    //! Путь текущего отрезка полёта, метры. Копится ВСЕГДА (иначе нечего было бы
    //! засчитать ретроактивно), а зачитывается только у помеченного отрезка.
    float currentAirborneMetres{};
    //! Помечен ли ТЕКУЩИЙ отрезок как планирование/двойной прыжок.
    bool currentStretchIsGlide{false};
    //! Путь ПРЕДЫДУЩЕГО, уже закончившегося отрезка, и момент приземления —
    //! для случая «отметка 0xe7 пришла уже на земле».
    float lastStretchMetres{};
    std::chrono::steady_clock::time_point lastLandingTimePoint{
      std::chrono::steady_clock::time_point::max()};
    //! Момент последней отметки DoubleJumpOrGlide — для случая «отметка пришла
    //! на пакет раньше взлёта». max() = отметки не было.
    std::chrono::steady_clock::time_point glideMarkTimePoint{
      std::chrono::steady_clock::time_point::max()};
    //! Рекорд планирования за заезд, метры. ★Сюда попадают ТОЛЬКО отрезки,
    //! ЗАКРЫВШИЕСЯ ПРИЗЕМЛЕНИЕМ: «вечно в воздухе» не должно конвертироваться
    //! в вечный рекорд.
    float longestGlideMetres{};

    //! Текущая цепочка ПЛАТНЫХ рывков и её максимум за заезд. ★В лошадь уходит
    //! МАКСИМУМ, а не последнее значение: серия из 12 рывков, оборванная у самой
    //! черты, обязана остаться двенадцатью.
    uint32_t boostCombo{};
    uint32_t boostComboMax{};
    //! Момент прошлого зачтённого рывка. max() = рывков ещё не было.
    std::chrono::steady_clock::time_point lastSpurTimePoint{
      std::chrono::steady_clock::time_point::max()};
  };

  //! An item deck.
  struct ItemDeck
  {
    //! An object identifier.
    Oid oid{};
    //! A list of items spawnable in this item deck.
    std::vector<uint32_t> items{};
    //!
    uint32_t currentItem{};
    //! A respawn time for the items in the deck.
    std::chrono::milliseconds respawnTime{};
    //! A time point of the next respawn.
    std::chrono::steady_clock::time_point respawnTimePoint{
      std::chrono::steady_clock::time_point::min()};
    protocol::Vector3 position{};
  };

  //! An event
  struct Event
  {
    uint32_t id{};
    std::chrono::steady_clock::time_point throttledUntil{};
  };

  struct TeamInfo
  {
    uint32_t points{0};
    uint32_t boostCount{0};
    bool gaugeLocked{false};
  };

  TeamInfo blueTeam{};
  TeamInfo redTeam{};

  //! A flag to indicate whether all items should be spawned.
  bool firstPassItemSpawn{true};

  //! An object map.
  using RacerObjectMap = std::map<data::Uid, Racer>;
  //! A deck item object map.
  using ItemDeckMap = std::map<Oid, ItemDeck>;
  //! An event map.
  using EventMap = std::unordered_map<uint32_t, Event>;

  //! Adds a racer for tracking.
  //! @param characterUid Character UID.
  //! @returns A reference to the racer record.
  Racer& AddRacer(data::Uid characterUid);

  //! Резервирует object id, НЕ создавая гонщика (R56, #61).
  //!
  //! ★Зачем отдельный метод. AI-соперники соло-заезда намеренно НЕ живут в
  //! трекере (см. RaceInstance::AiRacer — иначе синтетическая сущность попала
  //! бы во все ПИШУЩИЕ пути: награды, травмы, дейлики, телеметрию). Но их
  //! object id обязан быть уникален среди id живых гонщиков, иначе клиент
  //! спутает бота с игроком. Единственный способ это гарантировать —
  //! раздавать оба вида id ИЗ ОДНОГО счётчика, а не из «заведомо свободного
  //! диапазона»: заведомо свободных диапазонов не бывает.
  [[nodiscard]] Oid ReserveOid();
  //! Removes a racer from tracking.
  //! @param characterUid Character UID.
  void RemoveRacer(data::Uid characterUid);
  //! Returns whether the character is a racer.
  //! @param characterUid Character UID.
  //! @return `true` if the character is a racer,
  //!          otherwise returns `false`;
  bool IsRacer(data::Uid characterUid) const;
  //! Returns reference to the racer record.
  //! @returns Racer record.
  [[nodiscard]] Racer& GetRacer(data::Uid characterUid);
  //! Returns a reference to all racer records.
  //! @return Reference to racer records.
  [[nodiscard]] RacerObjectMap& GetRacers();

  //! Adds an item for tracking.
  //! @returns A reference to the new item record.
  ItemDeck& AddItemDeck();
  //! Removes an item from tracking.
  //! @param itemId Item OID.
  void RemoveItemDeck(Oid itemId);
  //! Returns reference to the item record.
  //! @param itemId Item OID.
  //! @returns Item record.
  [[nodiscard]] ItemDeck& GetItemDeck(Oid itemId);
  //! Returns a reference to all item records.
  //! @return Reference to item records.
  [[nodiscard]] ItemDeckMap& GetItemDecks();

  //! Returns a reference to all of the event records.
  //! @return Reference to event records.
  [[nodiscard]] EventMap& GetEvents();
  //! Checks and throttles an event.
  //! @param eventId Event ID.
  //! @returns True if event exists and is throttled, else event is tracked.
  bool IsEventThrottled(uint32_t eventId);

  //! Adds a per-racer event item for the given character.
  //! @returns Reference to the new event item record.
  EventItem& AddEventItem(data::Uid characterUid);
  //! Finds a per-racer event item by OID.
  //! @returns The OID if found, otherwise InvalidEntityOid.
  Oid FindEventItem(data::Uid characterUid, Oid oid);
  //! Returns reference to a per-racer event item by OID.
  //! @throws std::runtime_error if not found.
  [[nodiscard]] EventItem& GetEventItem(data::Uid characterUid, Oid oid);
  //! Removes a per-racer event item by OID.
  void RemoveEventItem(data::Uid characterUid, Oid oid);

  //! LOA-fix (R68, backlog #5/#99): добавляет гонщику квестовый предмет.
  //! ★БРОСАЕТ, если персонаж не гонщик (через `GetRacer`), — и это правильно:
  //! путь СЕРВЕРНЫЙ (раскладка на старте заезда), гонщик там существует по
  //! построению, а тихий пропуск скрыл бы поломку жизненного цикла заезда.
  //! @returns Ссылка на новую запись (oid уже выдан).
  QuestItem& AddQuestItem(data::Uid characterUid);
  //! Ищет квестовый предмет гонщика по OID.
  //! ★НЕ БРОСАЕТ ВООБЩЕ — ни на чужом oid, ни на не-гонщике (в отличие от
  //! `GetEventItem`): путь КЛИЕНТСКИЙ, и «такого предмета у тебя нет» там
  //! обычное дело (чужой oid, уже подобранный oid, дубликат пакета).
  //! @returns Указатель на запись либо `nullptr`.
  [[nodiscard]] QuestItem* FindQuestItem(data::Uid characterUid, Oid oid);
  //! Снимает квестовый предмет гонщика по OID. Тоже НЕ бросает: зовётся с того
  //! же клиентского пути сразу после `FindQuestItem`.
  void RemoveQuestItem(data::Uid characterUid, Oid oid);

  //! Returns the next object instance ID and increments the internal counter.
  //! @param increment The value to increment the internal counter by.
  //! @returns The next object instance ID before incrementing.
  uint16_t GetNextEffectInstanceIdAndIncrementBy(uint16_t increment);

  void Clear();

private:
  //! Mapping between character UIDs and their assigned OIDs.
  //! It's important these persist across races in a room as the client does not clear assignments internally. 
  std::unordered_map<data::Uid, Oid> _characterOids;
  //! Next OID for new character entities (100+).
  Oid _nextCharacterOid = 1;
  //! Next OID for item entities (1–99, reset each race).
  Oid _nextItemDeckOid = 1;
  //! Racer entities.
  RacerObjectMap _racers;
  //! Item deck entities.
  ItemDeckMap _itemDecks;
  //! Tracked race map events.
  EventMap _events;
  //! Next effect instance ID.
  uint16_t _nextEffectInstanceId = 0;
};

} // namespace server::tracker

#endif // RACETRACKER_HPP
