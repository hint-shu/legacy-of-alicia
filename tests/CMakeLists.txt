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

# R77. Both need the shipped horse tables, so they get the config dir by path.
# The tables ARE the change in this round: a test built against a fixture copy
# would go green while the shipped YAML rotted.
add_executable(registry_test_horse_tables)
target_sources(registry_test_horse_tables PRIVATE
        src/registry/TestHorseTables.cpp)
target_link_libraries(registry_test_horse_tables
        PRIVATE project-properties alicia-libserver)
target_compile_definitions(registry_test_horse_tables
        PRIVATE LOA_CONFIG_GAME_DIR="${CMAKE_SOURCE_DIR}/resources/config/game")

add_executable(ranch_test_genetics)
target_sources(ranch_test_genetics PRIVATE
        src/ranch/TestGenetics.cpp)
target_link_libraries(ranch_test_genetics
        PRIVATE project-properties alicia-libserver)
target_compile_definitions(ranch_test_genetics
        PRIVATE LOA_CONFIG_GAME_DIR="${CMAKE_SOURCE_DIR}/resources/config/game")

# LOA (R70, backlog #58): потолок доступных тиров + фильтр режима/состава.
add_executable(registry_test_achievement_tiers)
target_sources(registry_test_achievement_tiers PRIVATE
        src/registry/TestAchievementTiers.cpp)
target_link_libraries(registry_test_achievement_tiers
        PRIVATE project-properties alicia-libserver)
# Каталог достижений читается тестом ТОЛЬКО НА ЧТЕНИЕ — так проверка форм
# состава перестаёт зависеть от литералов в исходнике и ловит дрейф данных.
target_compile_definitions(registry_test_achievement_tiers
        PRIVATE LOA_ACHIEVEMENTS_CATALOG="${CMAKE_SOURCE_DIR}/resources/config/game/achievements.yaml")

# LOA (R70-fix-7, backlog #58): срок/потолок/порядок удержания попапов.
# ★ИСХОДНИК КЛАССА ПОДКЛЮЧАЕТСЯ НАПРЯМУЮ: он живёт в цели `alicia-server`
# (исполняемой), а не в библиотеке, и слинковать его иначе нечем. Класс не
# зависит ни от директора, ни от сети — только от протокольной структуры и
# `data::Uid`, поэтому пары файлов достаточно.
add_executable(ranch_test_achievement_notify_hold)
target_sources(ranch_test_achievement_notify_hold PRIVATE
        src/ranch/TestAchievementNotifyHold.cpp
        ${CMAKE_SOURCE_DIR}/src/server/ranch/AchievementNotifyHold.cpp)
target_include_directories(ranch_test_achievement_notify_hold
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ranch_test_achievement_notify_hold
        PRIVATE project-properties alicia-libserver)

add_test(NAME ProtocolTestMagic COMMAND protocol_test_magic)
add_test(NAME UtilTestStream COMMAND util_test_stream)
add_test(NAME UtilTestScheduler COMMAND util_test_scheduler)
add_test(NAME UtilTestLocale COMMAND util_test_locale)
add_test(NAME UtilTestAliciaShopTime COMMAND util_test_alicia_shop_time)
add_test(NAME UtilTestProfiler COMMAND util_test_profiler)
add_test(NAME UtilTestLogThrottle COMMAND util_test_log_throttle)
add_test(NAME RaceTestP2dIdPool COMMAND race_test_p2did_pool)
add_test(NAME RegistryTestHorseTables COMMAND registry_test_horse_tables)
add_test(NAME RanchTestGenetics COMMAND ranch_test_genetics)
add_test(NAME RegistryTestAchievementTiers COMMAND registry_test_achievement_tiers)
add_test(NAME RanchTestAchievementNotifyHold COMMAND ranch_test_achievement_notify_hold)


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
    # R77. Same reasoning as the two gates above: an oracle nobody runs is not an
    # oracle. The expected finding count is a property of the BRANCH -- 0 on the
    # candidate, non-zero on the data negatives -- so the round builder passes
    # --expect, while here it guards the tree that ships.
    add_test(NAME ToolDnaVsStockSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_dna_vs_stock.py --selftest)
    add_test(NAME ToolDnaVsStockGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_dna_vs_stock.py)
    # R77-fix-2 (Codex finding 6). The breeding potential roll lives in the
    # alicia-server EXECUTABLE and cannot be linked into a test binary, so neg-d
    # -- the negative that puts the uniform roll back -- was provable only by the
    # stand and the ladder, i.e. only for one SHA. This source gate states the
    # property durably; it proves itself on two fixtures before judging the tree.
    add_test(NAME ToolPotentialRollGate
            COMMAND bash ${CMAKE_SOURCE_DIR}/tools/check_potential_roll.sh)
else ()
    message(STATUS "Source gates (lobby auth, field init) are not wired on this platform")
endif ()
