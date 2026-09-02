// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: один нарушитель. `enum class` — скаляр: без инициализатора поле
// несёт мусор, который вдобавок не обязан быть ни одним из перечисленных
// значений.
#ifndef FIXTURE_ENUM_BAD_HPP
#define FIXTURE_ENUM_BAD_HPP

#include <cstdint>

namespace fixture
{

enum class Region : uint8_t
{
  Unknown = 0,
  Global = 1,
};

struct EnumBad
{
  Region region;
};

} // namespace fixture

#endif
