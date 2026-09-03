cmake_minimum_required(VERSION 3.20)

project(alicia-server)

option(BUILD_TESTS "Build tests" ON)

find_package(Boost 1.74.0 MODULE REQUIRED)

add_subdirectory(3rd-party SYSTEM)

include(cmake/Versioning.cmake)
get_semver()

# Version variables
set(AliciaServerBuildVersion ${ServerSemver})

# Generated version header.
configure_file(
        include/server/Version.hpp.in
        generated/Version.hpp)

# Project properties interface library
add_library(project-properties INTERFACE)
target_compile_features(project-properties
        INTERFACE cxx_std_23)

# Platform properties interface library
add_library(platform-properties INTERFACE)
if (${MSVC})
    target_compile_definitions(platform-properties INTERFACE
            _WIN32_WINNT=_WIN32_WINNT_WIN10
            NTDDI_VERSION=NTDDI_WIN10_19H1)
    target_link_libraries(platform-properties INTERFACE
            icu.lib)
elseif(MINGW)
    target_link_libraries(platform-properties INTERFACE
            icuuc icuio icuin ws2_32 mswsock)
else()
    target_link_libraries(platform-properties INTERFACE
            icuuc icui18n stdc++exp)
endif()

if (${MSVC})
    add_compile_options(/MP)
    target_compile_options(platform-properties INTERFACE
            /utf-8 /bigobj /W4 /permissive- /WX /wd4996 /wd4702)
else()
    target_compile_options(platform-properties INTERFACE
            -Wall -Wextra -Wpedantic -Wdeprecated-declarations)
endif()

# alicia-libserver target
add_library(alicia-libserver STATIC
        src/libserver/data/DataDirector.cpp
        src/libserver/data/DataRepair.cpp
        src/libserver/data/helper/ProtocolHelper.cpp
        src/libserver/data/file/FileDataSource.cpp
        #src/libserver/data/pq/PqDataSource.cpp
        src/libserver/network/Server.cpp
        src/libserver/network/chatter/proto/ChatterMessageDefinitions.cpp
        src/libserver/network/chatter/ChatterProtocol.cpp
        src/libserver/network/chatter/ChatterServer.cpp
        src/libserver/network/command/CommandProtocol.cpp
        src/libserver/network/command/CommandServer.cpp
        src/libserver/network/command/proto/CommonMessageDefinitions.cpp
        src/libserver/network/command/proto/CommonStructureDefinitions.cpp
        src/libserver/network/command/proto/LobbyMessageDefinitions.cpp
        src/libserver/network/command/proto/RaceMessageDefinitions.cpp
        src/libserver/network/command/proto/RanchMessageDefinitions.cpp
        src/libserver/network/http/WebSocket.cpp
        src/libserver/registry/BreedingOdds.cpp
        src/libserver/registry/CharacterRegistry.cpp
        src/libserver/registry/CourseRegistry.cpp
        src/libserver/registry/BreedingRegistry.cpp
        src/libserver/registry/HorseRegistry.cpp
        src/libserver/registry/ItemRegistry.cpp
        src/libserver/registry/MagicRegistry.cpp
        src/libserver/registry/PetRegistry.cpp
        src/libserver/registry/QuestRegistry.cpp
        src/libserver/registry/SystemContentRegistry.cpp
        src/libserver/registry/AchievementRegistry.cpp
        src/libserver/registry/CareSkillRegistry.cpp
        src/libserver/util/Locale.cpp
        src/libserver/util/Scheduler.cpp
        src/libserver/util/Stream.cpp
        src/libserver/util/Util.cpp
        src/libserver/util/Profiler.cpp
        src/libserver/util/LogThrottle.cpp
        src/libserver/util/KeyedLogThrottle.cpp)
target_include_directories(alicia-libserver PUBLIC
        include/)
target_link_libraries(alicia-libserver PRIVATE
        project-properties
        platform-properties)
target_link_libraries(alicia-libserver PUBLIC
        spdlog::spdlog
        Boost::headers
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
        zlibstatic
        tinyxml2)

# alicia-server target
add_executable(alicia-server
        src/server/authentication/AuthenticationService.cpp
        src/server/authentication/LocalAuthenticationBackend.cpp
        src/server/authentication/PostgresAuthenticationBackend.cpp
        src/server/main.cpp
        src/server/ServerInstance.cpp
        src/server/Config.cpp
        src/server/lobby/shop/Shop.cpp
        src/server/lobby/LobbyDirector.cpp
        src/server/lobby/LobbyNetworkHandler.cpp
        src/server/chat/AllChatDirector.cpp
        src/server/chat/PrivateChatDirector.cpp
        src/server/messenger/MessengerDirector.cpp
        src/server/race/MagicSystem.cpp
        src/server/race/RaceInstance.cpp
        src/server/race/RaceNetworkHandler.cpp
        src/server/ranch/BreedingMarket.cpp
        src/server/ranch/Genetics.cpp
        src/server/ranch/AchievementNotifyHold.cpp
        src/server/ranch/RanchDirector.cpp
        src/server/room/Room.cpp
        src/server/system/ChatSystem.cpp
        src/server/system/HorseSystem.cpp
        src/server/system/InfractionSystem.cpp
        src/server/system/ItemSystem.cpp
        src/server/system/ModerationSystem.cpp
        src/server/system/OtpSystem.cpp
        src/server/system/AchievementSystem.cpp
        src/server/system/QuestSystem.cpp
        src/server/system/RoomSystem.cpp
        src/server/system/MatchmakingSystem.cpp
        src/server/system/RewardSystem.cpp
        src/server/telemetry/Telemetry.cpp
        src/server/tracker/RaceTracker.cpp
        src/server/tracker/RanchTracker.cpp
        src/server/race/RaceDirector.cpp)
target_include_directories(alicia-server
        PRIVATE include/)
target_link_libraries(alicia-server PRIVATE
        project-properties
        platform-properties
        alicia-libserver
        libpqxx::pqxx)
target_include_directories(alicia-server PUBLIC
        "${PROJECT_BINARY_DIR}/generated")

# LOA-fix (R71-16, находка ревью 2 #9): ГЕЙТ ПОЛНОТЫ КЛАССИФИКАТОРА RELAY —
# ЧАСТЬ СБОРКИ, А НЕ СОВЕТ.
#
# `RelayAuthz.hpp` намеренно оставлен без `default:`, чтобы новый тип нагрузки
# ловился `-Wswitch`. Но предупреждение на Linux сборку не останавливает (`-Werror`
# стоит только у MSVC), поэтому забытый тип тихо стал бы неавторизованным каналом —
# ровно та дыра, которую раунд закрывает. Гейт `tools/check_relay_authz.sh` теперь
# ЦЕЛЬ СБОРКИ, от которой зависит `alicia-server`: не сошлось — не собралось.
#
# ★ЕСЛИ `bash` НЕ НАЙДЕН, ЭТО ГРОМКО. Молча пропавшая проверка — это ложно-зелёная
# сборка; предупреждение CMake остаётся в логе и видно на любой машине. Боевой путь
# (Dockerfile) и раундовый `tools/round/build_from_branch.sh` bash имеют всегда.
find_program(BASH_PROGRAM bash)
if (BASH_PROGRAM)
    add_custom_target(relay-authz-gate ALL
            COMMAND ${CMAKE_COMMAND} -E env "ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            ${BASH_PROGRAM} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_relay_authz.sh"
            COMMENT "relay-authz gate: каждый тип relay-нагрузки классифицирован"
            VERBATIM)
    add_dependencies(alicia-server relay-authz-gate)

    # LOA-fix (R71-27, находка ревью 5 #1): ГЕЙТ ТОТАЛЬНОГО ПРАВИЛА ДРОССЕЛЯ —
    # тоже часть сборки. Ревью нашло пять сырых жалоб в семействе наводки; сам
    # гейт нашёл ещё три вне его. Список мест закрывается один раз, правило —
    # навсегда ([[total-invariant-beats-list-of-sites]]).
    add_custom_target(race-rejection-throttle-gate ALL
            COMMAND ${CMAKE_COMMAND} -E env "ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            ${BASH_PROGRAM} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_race_rejection_throttle.sh"
            COMMENT "race-rejection-throttle gate: клиентская жалоба не пишется сырой"
            VERBATIM)
    add_dependencies(alicia-server race-rejection-throttle-gate)
else ()
    message(WARNING "bash не найден: гейты relay-authz и race-rejection-throttle НЕ включены в сборку")
endif ()

if (BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Adding -fexperimental-library for Clang compiler")
    target_compile_options(alicia-libserver
            PRIVATE -fexperimental-library)
    target_compile_options(alicia-server
            PRIVATE -fexperimental-library)
endif ()

add_custom_command(
        TARGET alicia-server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/resources
        ${CMAKE_CURRENT_BINARY_DIR})
install(TARGETS alicia-server)
