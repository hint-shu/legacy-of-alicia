// f1 — P1: сырой узкий счётчик в МЕТОДЕ. Базовая форма дефекта.
#include "fixtures.hpp"

void FixtureOne::Write(const FixtureCommand& command, SinkStream& stream)
{
  stream.Write(static_cast<uint8_t>(command.items.size()));
}
