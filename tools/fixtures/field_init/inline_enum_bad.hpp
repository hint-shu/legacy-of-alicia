// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix2-3 (находка Codex 3, итерация 2). Ожидание: ровно один нарушитель —
// поле `part` на строке 28. Перечисление объявлено ПРЯМО В ОБЪЯВЛЕНИИ ПОЛЯ, и
// закрывающая скобка его тела уносила тип из разбираемого объявления: до
// судьи доезжал голый декларатор `part`, который не разбирался и молча
// выбрасывался. Ровно эта форма прятала настоящее поле в ItemRegistry.hpp.
#ifndef FIXTURE_INLINE_ENUM_BAD_HPP
#define FIXTURE_INLINE_ENUM_BAD_HPP

#include <cstdint>

namespace fixture
{

struct InlineEnumBad
{
  //! Инициализированный — не нарушитель.
  enum class Kind
  {
    First = 0,
    Second = 1
  } kind{Kind::First};

  enum class Part
  {
    Body = 0,
    Mane = 1
  } part;
};

} // namespace fixture

#endif
