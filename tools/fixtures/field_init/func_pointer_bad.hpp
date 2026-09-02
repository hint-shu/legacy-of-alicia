// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix-5 (находка Codex 5). Ожидание: ровно один нарушитель — поле
// `callback` на строке 18. Указатель на функцию — такой же неинициализированный
// скаляр, как любой другой, но первая редакция гарда выбрасывала его вместе с
// объявлениями методов по одному лишь признаку «в строке есть скобка».
#ifndef FIXTURE_FUNC_POINTER_BAD_HPP
#define FIXTURE_FUNC_POINTER_BAD_HPP

#include <cstdint>

namespace fixture
{

struct FuncPointerBad
{
  //! Инициализированный указатель на функцию — не нарушитель.
  void (*ready)(){nullptr};
  void (*callback)();
  //! Объявление метода рядом — по-прежнему НЕ поле.
  uint32_t GetValue() const;
};

} // namespace fixture

#endif
