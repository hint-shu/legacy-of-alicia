/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2026 Story Of Alicia
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

#ifndef SERVER_RACE_MAGIC_APPLICATION_HPP
#define SERVER_RACE_MAGIC_APPLICATION_HPP

#include <cstddef>
#include <cstdint>

namespace server::race
{

//! LOA-fix (R71-22, находки ревью 3 #1 и #2; ПЕРЕПИСАНО R71-25 по находке ревью 4 #1):
//! КТО ИМЕННО ПРИМЕНЯЕТ ЭФФЕКТ.
//!
//! Раунд опирался на «зарегистрирован ли номер экземпляра» и не спрашивал главного:
//! КТО вешает этот эффект. А ответ у магии ровно трёх видов, и он определяет, что
//! вообще имеет право сделать клиентский отчёт «на мне сработало»:
//!
//!  * `ServerAppliedAtCast` — щиты, бустеры, разгон и командные бафы. Их вешает САМ
//!    сервер в момент каста (`HandleUseMagicItem`, switch по типу). Клиентский отчёт
//!    про них не нужен НИКОГДА: он либо повтор, либо кража чужого каста себе.
//!  * `IceWallObstacle` — препятствие на трассе. Отчёт про него значит «я её сломал»,
//!    и его вправе прислать любой, кто в стену въехал.
//!  * `TargetReportedAttack` — атака по гонщику. Геометрии у сервера нет, поэтому
//!    отчёт жертвы — ЕДИНСТВЕННЫЙ путь применения; всё, что он может сделать, —
//!    навредить самому отправителю (гард R71-4 не пускает эффект на чужого).
//!  * `Unknown` — тип, о котором это правило НИЧЕГО не знает. Не «наверное атака»:
//!    ни выдать экземпляр, ни принять отчёт по такому типу нельзя.
enum class MagicApplication : uint8_t
{
  ServerAppliedAtCast,
  IceWallObstacle,
  TargetReportedAttack,
  Unknown,
};

//! LOA-fix (R71-25, находка ревью 4 #1): ПОЛНАЯ ШИРИНА И ЗАКРЫТЫЙ ХВОСТ.
//!
//! ★ЧТО БЫЛО СЛОМАНО. Классификатор принимал `uint16_t`, хотя тип магии в реестре —
//! `uint32_t` (`MagicRegistry.hpp:39`), и такой же `uint32_t` уезжает в протокол
//! (`AcCmdRCMagicExpire::magicType`). `magic.yaml` лежит на прод-хосте bind-mount'ом,
//! то есть значение типа НЕ ограничено кодом: запись `type: 0x10002` сужалась до `2`
//! и становилась неотличима от FireBall'а — при том, что switch каста видел полное
//! значение и не делал ничего. Обе половины сверки при этом говорили «не серверный»
//! и МОЛЧАЛИ: гейт сходился по форме и был слеп к содержанию.
//! Здесь ширина одна и та же везде, где тип хранится или сравнивается
//! (`RaceTracker::EffectInstance::magicType` — тоже `uint32_t`).
//!
//! ★ХВОСТ ЗАКРЫТ. Раньше `return TargetReportedAttack` стоял терминальным
//! fallthrough'ом — то есть ЛЮБОЙ неизвестный тип по умолчанию получал права атаки
//! (запись экземпляра + клиентский отчёт + рассылка). Теперь семейства перечислены
//! поимённо, а всё остальное — `Unknown`, и вызывающий обязан ОТКАЗАТЬ: и до выдачи
//! экземпляра, и до рассылки. Фейл-оупен по умолчанию — это ровно тот класс, ради
//! которого затеян раунд.
//!
//! ★ЧТО ЛОМАЕТСЯ, ЕСЛИ ОШИБЛИСЬ. Перечислены все 25 типов поставляемого
//! `magic.yaml`, КРОМЕ 27 («Positional magic (???)»): у него `skillEffectId: 99999`
//! (слота под такой эффект не существует — `IsSchedulableEffectId`), а все восемь
//! `positionalWeights` нулевые, поэтому `std::discrete_distribution` не выдаёт его
//! никогда. То есть сегодня отказ по нему недостижим; появись у него вес — отказ
//! будет ГРОМКИМ (дросселированная жалоба с номером типа), а не тихой «атакой».
[[nodiscard]] constexpr MagicApplication ClassifyMagicApplication(
  const uint32_t magicType) noexcept
{
  switch (magicType)
  {
    // WaterShield 4/5, Booster 6/7, HotRodding 8/9 — сервер вешает сам при касте.
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    // BufPower 20/21, BufGauge 22/23, BufSpeed 24/25 — то же самое, командные бафы.
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
      return MagicApplication::ServerAppliedAtCast;
    // IceWall 10/11 — препятствие на трассе.
    case 10:
    case 11:
      return MagicApplication::IceWallObstacle;
    // FireBall 2/3, JumpStun 12/13, DarkFire 14/15, Summon 16/17, Lightning 18/19 —
    // атаки: применяются только по отчёту цели.
    case 2:
    case 3:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
      return MagicApplication::TargetReportedAttack;
    default:
      return MagicApplication::Unknown;
  }
}

//! LOA-fix (R71-28, находка ревью 5 #2, BLOCK): ЕДИНСТВЕННЫЕ ДВЕ ФОРМЫ СПИСКА
//! ЛЕДЯНОЙ СТЕНЫ.
//!
//! У стены `AcCmdCRUseMagicItem::targetList` — это сосульки, а не гонщики, и размеров
//! у него ровно два: обычная стена ставит ОДНУ (список `[2]`), критическая — ТРИ
//! (`[1, 2, 3]`); так это записано в определении сообщения
//! (`RaceMessageDefinitions.hpp:1942-1944`) и так же выглядят захваты r41/r53.
//! Размер читается `uint8_t` (`RaceMessageDefinitions.cpp:1537-1541`), то есть протокол
//! принимает и НОЛЬ, и 255 — обе величины у честного клиента невозможны.
//!
//! ★ЖИВЁТ ЗДЕСЬ, А НЕ В ХЕНДЛЕРЕ, чтобы форму можно было проверить юнит-тестом:
//! проверка, которую нельзя прогнать отдельно, проверяется только глазами
//! ([[a-gate-must-prove-itself-first]]).
inline constexpr size_t IceWallSegmentsNormal = 1;
inline constexpr size_t IceWallSegmentsCritical = 3;

//! @returns `true`, если `segmentCount` — законная форма списка ледяной стены.
[[nodiscard]] constexpr bool IsKnownIceWallSegmentCount(const size_t segmentCount) noexcept
{
  return segmentCount == IceWallSegmentsNormal
    || segmentCount == IceWallSegmentsCritical;
}

} // namespace server::race

#endif // SERVER_RACE_MAGIC_APPLICATION_HPP
