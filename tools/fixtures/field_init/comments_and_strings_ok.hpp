/**
 * Фикстура гарда tools/check_field_init.py. НЕ КОД СЕРВЕРА.
 *
 * Лицензионная шапка нарочно сделана длинной (как у настоящих файлов), потому
 * что она и есть ловушка: гард, который ВЫРЕЗАЕТ комментарии вместо того, чтобы
 * заменить их пробелами, сдвигает все последующие номера строк ровно на её
 * высоту — и потом уверенно отправляет читателя чинить чужую строку. Прототип
 * этого гарда именно так печатал CourseRegistry.hpp:83 для поля, живущего
 * на :100.
 *
 * Ожидание: НОЛЬ нарушителей (все «поля» ниже спрятаны в комментариях и в
 * строковом литерале), И номер строки поля `probe` в точности совпадает с
 * записанным в SELFTEST_EXPECTATIONS.
 *
 * This program is free software; you can redistribute it and/or modify it.
 **/
#ifndef FIXTURE_COMMENTS_AND_STRINGS_OK_HPP
#define FIXTURE_COMMENTS_AND_STRINGS_OK_HPP

#include <cstdint>
#include <string>

namespace fixture
{

struct CommentsAndStringsOk
{
  // uint32_t hiddenInLineComment;
  /* uint32_t hiddenInBlockComment; */
  /*
   * uint32_t hiddenInMultilineComment;
   */
  //! Строковый литерал со значимой точкой с запятой внутри.
  std::string sql{"SELECT 1; SELECT uint32_t hiddenInString;"};

  //! ★ЯКОРЬ НОМЕРА СТРОКИ. Инициализировано, то есть не нарушитель; проверяется
  //! именно НОМЕР, потому что чистая фикстура не может доказать это списком
  //! нарушителей.
  uint32_t probe{0};
};

} // namespace fixture

#endif
