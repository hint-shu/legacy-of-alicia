// Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
// R72-fix4-4 (находка Codex 4, итерация 4). ★ПОЛОЖИТЕЛЬНАЯ ПОЛОВИНА ПАРЫ:
// объявление, которое ДЕЙСТВИТЕЛЬНО называет оператор, обязано считаться методом.
// Сломай фильтр операторов — и `Probe& operator=(const Probe&);` уедет не в
// «метод», а в «поле с инициализатором» (потому что `=` принадлежит ИМЕНИ, а
// разбор режет объявление по первому `=`), при этом код останется 0 и список
// нарушителей пустым. Поэтому фикстура пинует СЧЁТЧИКИ: полей 0, методов 4.
// Ожидание: код 0, полей 0, методов 4, неразобранных 0.
#ifndef FIXTURE_OPERATOR_METHOD_OK_HPP
#define FIXTURE_OPERATOR_METHOD_OK_HPP

#include <cstdint>

namespace fixture
{

struct OperatorMethodOk
{
  bool operator==(const OperatorMethodOk& other) const;
  bool operator<(const OperatorMethodOk& other) const;
  OperatorMethodOk& operator=(const OperatorMethodOk& other);
  uint32_t operator[](uint32_t index) const;
};

} // namespace fixture

#endif
