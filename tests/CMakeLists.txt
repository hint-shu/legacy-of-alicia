include(CTest)

add_executable(protocol_test_magic)
target_sources(protocol_test_magic PRIVATE
        src/protocol/TestMagic.cpp)
target_link_libraries(protocol_test_magic
        PRIVATE project-properties alicia-libserver)

# R74. Счётчик списка на проводе и бюджет блока макросов — свойства
# СЕРИАЛИЗАТОРА, а стенд видит их только через клиента и только для тех
# сообщений, которые арка успевает вызвать. Здесь судится сам примитив:
# порядок клампа (264 при бонде 16 -> 16, а не 8), откат курсора при нехватке
# места, масштаб счёта, байтовая тождественность на данных в пределах бонда и
# ИЗМЕРЕННЫЕ бюджеты двух тяжёлых кадров.
add_executable(protocol_test_bounded_list)
target_sources(protocol_test_bounded_list PRIVATE
        src/protocol/TestBoundedList.cpp)
target_link_libraries(protocol_test_bounded_list
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

add_executable(util_test_keyed_log_throttle)
target_sources(util_test_keyed_log_throttle PRIVATE
        src/util/TestKeyedLogThrottle.cpp)
target_link_libraries(util_test_keyed_log_throttle
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_name_guard)
target_sources(util_test_name_guard PRIVATE
        src/util/TestNameGuard.cpp)
target_link_libraries(util_test_name_guard
        PRIVATE project-properties alicia-libserver)

add_executable(util_test_atomic_file)
target_sources(util_test_atomic_file PRIVATE
        src/util/TestAtomicFile.cpp)
target_link_libraries(util_test_atomic_file
        PRIVATE project-properties alicia-libserver)

add_executable(data_test_file_data_source)
target_sources(data_test_file_data_source PRIVATE
        src/data/TestFileDataSource.cpp)
target_link_libraries(data_test_file_data_source
        PRIVATE project-properties alicia-libserver)

add_executable(race_test_relay_authz)
target_sources(race_test_relay_authz PRIVATE
        src/race/TestRelayAuthz.cpp)
target_link_libraries(race_test_relay_authz
        PRIVATE project-properties alicia-libserver)

add_executable(protocol_test_relay_payload)
target_sources(protocol_test_relay_payload PRIVATE
        src/protocol/TestRelayPayload.cpp)
target_link_libraries(protocol_test_relay_payload
        PRIVATE project-properties alicia-libserver)

# LOA-fix (R71-22, находки ревью 3 #1 и #5): правила реестра экземпляров эффектов
# проверяются юнит-тестом. `RaceTracker.cpp` не лежит в `alicia-libserver` (он часть
# исполняемого сервера), поэтому компилируется в тест напрямую — зависимостей у него
# ровно две: собственный заголовок и `<limits>`.
add_executable(race_test_effect_instances)
target_sources(race_test_effect_instances PRIVATE
        src/race/TestEffectInstances.cpp
        ${CMAKE_SOURCE_DIR}/src/server/tracker/RaceTracker.cpp)
target_include_directories(race_test_effect_instances PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(race_test_effect_instances
        PRIVATE project-properties alicia-libserver)

# LOA-fix (R71-25, находка ревью 4 #1): классификация типа магии — заголовок без
# зависимостей, поэтому тест собирается из него одного.
add_executable(race_test_magic_application)
target_sources(race_test_magic_application PRIVATE
        src/race/TestMagicApplication.cpp)
target_include_directories(race_test_magic_application PRIVATE
        ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(race_test_magic_application
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

# LOA (R70-fix-8, backlog #58, находка Codex 6 BLOCK-2): строгий разбор
# значений строгих ключей конфига. ★Заголовочный, поэтому линковать нечего,
# кроме include-пути: правило разбора вынесено из Config.cpp ИМЕННО ради того,
# чтобы его можно было проверить без поднятия сервера.
add_executable(server_test_config_strict)
target_sources(server_test_config_strict PRIVATE
        src/server/TestConfigStrict.cpp)
target_include_directories(server_test_config_strict
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(server_test_config_strict
        PRIVATE project-properties)

# R75. Пер-курсовые рекорды — новые поля ЗАПИСИ на диске, и круг
# «записали -> прочитали» на стенде не проверяется: стенд пишет и читает одним
# сервером, где потеря при сохранении и потеря при чтении сокращаются.
add_executable(data_test_course_records)
target_sources(data_test_course_records PRIVATE
        src/data/TestCourseRecords.cpp)
target_link_libraries(data_test_course_records
        PRIVATE project-properties alicia-libserver)

# R76. Журнал трассы: накопление живёт в RaceNetworkHandler, печать — в
# RaceInstance, обе цели в тестовый бинарь не линкуются, и обе доказываются
# стендом. Здесь — ЧЕТЫРЕ свойства, которых стенд не видит: самоинициализация
# полей журнала на замусоренном хранилище, недостижимость сентинела «порог не
# взят», 64-битный знаменатель проверки плотности и достижимость самих порогов
# раунда 0. Заголовочно-структурный тест, линкуется к libserver.
add_executable(race_test_race_journal)
target_sources(race_test_race_journal PRIVATE
        src/race/TestRaceJournal.cpp)
target_include_directories(race_test_race_journal
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(race_test_race_journal
        PRIVATE project-properties alicia-libserver)
# LOA (R78, round78, backlog #255): правило вытеснения устаревшей
# мессенджер-сессии. ★Заголовочное, поэтому линковать нечего, кроме
# include-пути: правило вынесено из MessengerDirector.cpp ИМЕННО затем, чтобы
# его можно было проверить без поднятия сервера.
add_executable(messenger_test_session_eviction)
target_sources(messenger_test_session_eviction PRIVATE
        src/messenger/TestSessionEviction.cpp)
target_include_directories(messenger_test_session_eviction
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(messenger_test_session_eviction
        PRIVATE project-properties)

# LOA (R78, round78, backlog #255): два вида ключей выдачи. Раунд не меняет
# OtpSystem, но целиком стоит на его свойствах — в том числе на том, что
# ОДНОРАЗОВЫЙ код остался одноразовым для ранча и заезда.
add_executable(server_test_otp_system)
target_sources(server_test_otp_system PRIVATE
        src/server/TestOtpSystem.cpp
        ${CMAKE_SOURCE_DIR}/src/server/system/OtpSystem.cpp)
target_include_directories(server_test_otp_system
        PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(server_test_otp_system
        PRIVATE project-properties)

add_test(NAME ProtocolTestMagic COMMAND protocol_test_magic)
add_test(NAME ProtocolTestBoundedList COMMAND protocol_test_bounded_list)
add_test(NAME UtilTestStream COMMAND util_test_stream)
add_test(NAME UtilTestScheduler COMMAND util_test_scheduler)
add_test(NAME UtilTestLocale COMMAND util_test_locale)
add_test(NAME UtilTestAliciaShopTime COMMAND util_test_alicia_shop_time)
add_test(NAME UtilTestProfiler COMMAND util_test_profiler)
add_test(NAME UtilTestLogThrottle COMMAND util_test_log_throttle)
add_test(NAME UtilTestKeyedLogThrottle COMMAND util_test_keyed_log_throttle)
add_test(NAME UtilTestNameGuard COMMAND util_test_name_guard)
add_test(NAME UtilTestAtomicFile COMMAND util_test_atomic_file)
add_test(NAME DataTestFileDataSource COMMAND data_test_file_data_source)
add_test(NAME RaceTestP2dIdPool COMMAND race_test_p2did_pool)
add_test(NAME RegistryTestHorseTables COMMAND registry_test_horse_tables)
add_test(NAME RanchTestGenetics COMMAND ranch_test_genetics)
add_test(NAME RegistryTestAchievementTiers COMMAND registry_test_achievement_tiers)
add_test(NAME RanchTestAchievementNotifyHold COMMAND ranch_test_achievement_notify_hold)
add_test(NAME ServerTestConfigStrict COMMAND server_test_config_strict)
add_test(NAME DataTestCourseRecords COMMAND data_test_course_records)
add_test(NAME RaceTestRaceJournal COMMAND race_test_race_journal)
add_test(NAME RaceTestEffectInstances COMMAND race_test_effect_instances)
add_test(NAME RaceTestMagicApplication COMMAND race_test_magic_application)
add_test(NAME RaceTestRelayAuthz COMMAND race_test_relay_authz)
add_test(NAME ProtocolTestRelayPayload COMMAND protocol_test_relay_payload)
add_test(NAME MessengerTestSessionEviction COMMAND messenger_test_session_eviction)
add_test(NAME ServerTestOtpSystem COMMAND server_test_otp_system)


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
    # R75. Both are DELTA gates: they judge the lines this round ADDED, not the
    # file's contents, because the properties ("no record access added inside
    # Stop()", "the position handler acquired no logging") do not exist as
    # properties of a file -- Stop() already holds twelve direct accesses the
    # round keeps, and the position handler already holds one pre-existing debug
    # line. Each proves it can fail on three fixtures before judging anything,
    # and each stops with code 2 rather than reporting clean when it is blind.
    # The self-tests run here; the delta run needs a BASE and lives in the round
    # builder, since "what this branch added" is a property of the BRANCH.
    add_test(NAME ToolR75RecordAccessSelftest
            COMMAND bash ${CMAKE_SOURCE_DIR}/tools/r75_record_access_gate.sh --self-test)
    add_test(NAME ToolR75UserPosLoggingSelftest
            COMMAND bash ${CMAKE_SOURCE_DIR}/tools/r75_userpos_logging_gate.sh --self-test)
    # R76. The track journal's two invariants are properties of the SOURCE, not
    # of a binary: the position handler must acquire no logging (a remote-driven
    # path that once produced 15350 error lines an hour), and every journal field
    # must be reset per race. Neither can be observed on the stand -- the first
    # because a debug line changes nothing an arc can see, the second because
    # today the tracker is cleared wholesale anyway and the reset is a guard for
    # the day someone pools racers instead.
    # The gate extracts the field names BY PROPERTY from the header block rather
    # than carrying a list, so a ninth field is covered the moment it is added;
    # the count is not hardcoded, only a blindness floor is. It proves itself on
    # four fixtures -- two violations and two blindnesses -- before judging.
    add_test(NAME ToolTrackJournalSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/round/check_track_journal.py
                    ${CMAKE_SOURCE_DIR} --self-test)
    add_test(NAME ToolTrackJournalGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/round/check_track_journal.py
                    ${CMAKE_SOURCE_DIR})
    # LOA (R78-fix3, round78, backlog #255, находка Codex 2 итерации 2): гейт
    # замка над картой клиентов мессенджера. ★ЕДИНСТВЕННАЯ проверка раунда,
    # которая умеет покраснеть на пропущенной синхронизации: гонку данных стенд
    # детерминированно не воспроизводит, поэтому проверка структурная — перепись
    # обращений к карте и требование замка там, где он обязан быть. Гейт сперва
    # доказывает себя: снимает каждый из пяти замков по очереди и требует, чтобы
    # покраснел ровно тот.
    add_test(NAME ToolMessengerClientMapLockSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_messenger_client_map_lock.py --selftest)
    add_test(NAME ToolMessengerClientMapLockGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_messenger_client_map_lock.py)
    # LOA (R78-fix9, round78, backlog #255, находка ревю #4 BLOCK): под замком
    # карты клиентов не должно быть НИ ОДНОГО выхода, который сам берёт этот же
    # замок или синхронно рвёт соединение. ★ДЕТЕРМИНИРОВАННЫЙ фальсификатор
    # BLOCK'а: стендовая ячейка `mid-login-revoke` ловит то же окно ГОНКОЙ, то
    # есть вероятностно, а этот гейт краснеет всегда — проверено на дереве
    # ДО фикса (нарушение на той самой строке, которую назвало ревю).
    add_test(NAME ToolMessengerDisconnectOutsideLockSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_messenger_disconnect_outside_lock.py --selftest)
    add_test(NAME ToolMessengerDisconnectOutsideLockGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_messenger_disconnect_outside_lock.py)
    # R74. Гейт ограниченных списков ключится на СВОЙСТВО («функция, среди
    # параметров которой есть SinkStream&»), а не на написание `::Write(` — та
    # формулировка не видела площадку в свободной функции, и дыра пережила бы
    # раунд. Самопроверка на семи фикстурах доказывает, что он умеет краснеть,
    # ДО того, как он что-либо судит; боевой прогон живёт в сборщике раунда,
    # потому что негативная ветка обязана оставаться собираемой.
    # ★R74-fix-2: боевой прогон этих двух живёт в `tools/round/Dockerfile.tests`
    # (канон раунда с 52750a0b) — своей ступени `tests` раунд больше не держит.
    add_test(NAME ToolCheckBoundedListsSelftest
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_bounded_lists.py --self-test
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME ToolCheckBoundedListsGate
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_bounded_lists.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else ()
    message(STATUS "Source gates (lobby auth, field init) are not wired on this platform")
endif ()
# LOA-fix (R71-16): тот же гейт, но как ТЕСТ — чтобы `ctest` тоже краснел, а не
# только сборка. Две точки вызова ловят разное: цель сборки останавливает выкат,
# тест показывает вердикт в отчёте прогонов.
if (BASH_PROGRAM)
    add_test(NAME RelayAuthzGate
            COMMAND ${BASH_PROGRAM} "${CMAKE_SOURCE_DIR}/tools/check_relay_authz.sh")
    set_tests_properties(RelayAuthzGate PROPERTIES
            ENVIRONMENT "ROOT=${CMAKE_SOURCE_DIR}")
endif ()
