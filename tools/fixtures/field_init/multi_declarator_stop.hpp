// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix2-3 (находка Codex 3, итерация 2). Ожидание: КОД 2 (проверка
// недействительна). `uint32_t a, b;` объявляет ДВА скалярных поля; разбирать
// общий случай нескольких деклараторов (у каждого свои `*`, `[]` и свой
// инициализатор) — это писать парсер деклараторов. Гард обязан не молчать:
// он останавливается и требует разнести объявление по одному полю на строку.
#ifndef FIXTURE_MULTI_DECLARATOR_STOP_HPP
#define FIXTURE_MULTI_DECLARATOR_STOP_HPP

#include <cstdint>

namespace fixture
{

struct MultiDeclaratorStop
{
  uint32_t a, b;
};

} // namespace fixture

#endif
