// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: один нарушитель — скаляр во ВЛОЖЕННОЙ структуре. Наивный обход
// «только верхний уровень» его не увидит.
#ifndef FIXTURE_NESTED_SCALAR_BAD_HPP
#define FIXTURE_NESTED_SCALAR_BAD_HPP

#include <cstdint>

namespace fixture
{

struct NestedScalarBad
{
  uint32_t outer{0};

  struct Inner
  {
    uint32_t inner;
  };

  Inner value{};
};

} // namespace fixture

#endif
