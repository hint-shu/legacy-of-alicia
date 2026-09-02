// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix2-3 (находка Codex 3, итерация 2). Ожидание: ровно один нарушитель —
// поле `flags` на строке 19. `uint32_t (flags);` — законное объявление
// СКАЛЯРНОГО поля: скобки стоят вокруг имени, а не вокруг списка параметров.
// Оно кончается на `)` так же, как объявление метода, и предыдущая редакция
// гарда засчитывала его в «методы» — то есть поле уходило из-под проверки.
#ifndef FIXTURE_PAREN_DECLARATOR_BAD_HPP
#define FIXTURE_PAREN_DECLARATOR_BAD_HPP

#include <cstdint>

namespace fixture
{

struct ParenDeclaratorBad
{
  //! Инициализированный — не нарушитель.
  uint32_t (ready){0};
  uint32_t (flags);
  //! Метод с БЕЗЫМЯННЫМ параметром — та же форма, но это НЕ поле:
  //! `uint32_t` в скобках — имя типа, а не имя декларатора.
  void Handle(uint32_t);
};

} // namespace fixture

#endif
