// f2 — P1 в СВОБОДНОЙ ФУНКЦИИ. Ловушка на ключ поиска: перепись, ключившаяся
// на `::Write(`, эту площадку не видела, а счётчик отсюда уезжает каждому
// входящему в комнату (`WritePlayerRacer.equipment`).
#include "fixtures.hpp"

void WriteFixtureAvatar(SinkStream& stream, const FixtureAvatar& playerRacer)
{
  stream.Write(static_cast<uint8_t>(playerRacer.equipment.size()));
}
