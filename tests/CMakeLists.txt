include(CTest)

add_executable(protocol_test_magic)
target_sources(protocol_test_magic PRIVATE
        src/protocol/TestMagic.cpp)
target_link_libraries(protocol_test_magic
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_stream)
target_sources(util_test_stream PRIVATE
        src/util/TestStream.cpp)
target_link_libraries(util_test_stream
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_scheduler)
target_sources(util_test_scheduler PRIVATE
        src/util/TestScheduler.cpp)
target_link_libraries(util_test_scheduler
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_locale)
target_sources(util_test_locale PRIVATE
        src/util/TestLocale.cpp)
target_link_libraries(util_test_locale
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_alicia_shop_time)
target_sources(util_test_alicia_shop_time PRIVATE
        src/util/TestAliciaShopTime.cpp)
target_link_libraries(util_test_alicia_shop_time
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_profiler)
target_sources(util_test_profiler PRIVATE
        src/util/TestProfiler.cpp)
target_link_libraries(util_test_profiler
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_log_throttle)
target_sources(util_test_log_throttle PRIVATE
        src/util/TestLogThrottle.cpp)
target_link_libraries(util_test_log_throttle
        PRIVATE project-properties alicia-libserver)

add_executable(race_test_p2did_pool)
target_sources(race_test_p2did_pool PRIVATE
        src/race/TestP2dIdPool.cpp)
target_link_libraries(race_test_p2did_pool
        PRIVATE project-properties alicia-libserver)

add_test(NAME ProtocolTestMagic COMMAND protocol_test_magic)
add_test(NAME UtilTestStream COMMAND util_test_stream)
add_test(NAME UtilTestScheduler COMMAND util_test_scheduler)
add_test(NAME UtilTestLocale COMMAND util_test_locale)
add_test(NAME UtilTestAliciaShopTime COMMAND util_test_alicia_shop_time)
add_test(NAME UtilTestProfiler COMMAND util_test_profiler)
add_test(NAME UtilTestLogThrottle COMMAND util_test_log_throttle)
add_test(NAME RaceTestP2dIdPool COMMAND race_test_p2did_pool)


# LOA-fix (R72-fix-2, round72, находка Codex 3): ★ГАРДЫ ИСХОДНИКОВ — НА ОБЫЧНОМ
# ТЕСТОВОМ ПУТИ, А НЕ «ЗАПУСКАЮТСЯ РУКАМИ РАУНДА».
#
# Проверка, чей вердикт никто не читает, — не проверка. Оба гарда до этой правки
# не были подключены никуда: ни к CMake, ни к тестам, ни к сборке образа, — то
# есть жили ровно один раунд, а дальше «зелены» по умолчанию.
#
# ★ПОЧЕМУ ctest, А НЕ ЦЕЛЬ ALL. Раунд собирает НЕГАТИВНЫЕ образы — ветки, в
# которых защита снята намеренно, чтобы стенд увидел разницу. Гард в цели ALL
# сделал бы такой образ несобираемым, и доказательство «без защиты плохо» стало
# бы недоступно. Ожидаемый вердикт гарда — свойство ВЕТКИ, и живёт он в
# контракте сборщика раунда; здесь же гард стоит там, где его читает всякий, кто
# вообще запускает тесты.
if (NOT WIN32)
    add_test(NAME ToolLobbyAuthGate
            COMMAND bash ${CMAKE_SOURCE_DIR}/tools/check_lobby_auth_gate.sh)
    add_test(NAME ToolFieldInitSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_field_init.py --selftest)
    add_test(NAME ToolFieldInitGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_field_init.py)
else ()
    message(STATUS "Source gates (lobby auth, field init) are not wired on this platform")
endif ()
