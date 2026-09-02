// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: один нарушитель на ТРЕТЬЕМ уровне вложенности — ровно форма
// Course::MapBlockInfo::DeckItemInstance из настоящего дефекта #174.
#ifndef FIXTURE_DOUBLE_NESTED_BAD_HPP
#define FIXTURE_DOUBLE_NESTED_BAD_HPP

#include <cstdint>

namespace fixture
{

struct DoubleNestedBad
{
  struct Middle
  {
    struct Innermost
    {
      uint32_t deep;
    };

    Innermost innermost{};
  };

  Middle middle{};
};

} // namespace fixture

#endif
