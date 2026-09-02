// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// Ожидание: ровно один нарушитель — поле `a` на строке, названной в
// SELFTEST_EXPECTATIONS. Простейший случай: скаляр на верхнем уровне
// структуры, без инициализатора.
#ifndef FIXTURE_TOP_SCALAR_BAD_HPP
#define FIXTURE_TOP_SCALAR_BAD_HPP

#include <cstdint>

namespace fixture
{

struct TopScalarBad
{
  //! Инициализированное поле — не нарушитель.
  uint32_t ok{0};
  //! А это — нарушитель.
  uint32_t a;
};

} // namespace fixture

#endif
