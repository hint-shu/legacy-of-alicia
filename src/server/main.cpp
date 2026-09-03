/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2024 Story Of Alicia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **/

#include "Version.hpp"
#include "libserver/util/QuietLog.hpp"
#include "server/ServerInstance.hpp"
#include "server/ConfigStrict.hpp"
#include <libserver/util/Util.hpp>

#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <iostream>
#include <memory>

#ifdef WIN32
  #include <windows.h>
#else
  #include <unistd.h>
  #include <signal.h>
#endif

namespace
{

using Clock = std::chrono::steady_clock;

std::atomic_bool shouldProgramRun = true;
// LOA-fix (R65-1, backlog #200): запись в atomic из обработчика сигнала законна
// только если он БЕЗ ЗАМКА. Утверждение проверяет КОМПИЛЯТОР, а не память
// читающего: комментарий «здесь всё в порядке» не умеет провалиться.
static_assert(
  std::atomic_bool::is_always_lock_free,
  "the shutdown flag is written from a signal handler and has to be lock-free");
std::condition_variable shouldProgramRunCv;

std::shared_ptr<spdlog::logger> g_logger;

Clock::time_point serverStartupTime;

#ifdef WIN32

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
  switch (fdwCtrlType)
  {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
      {
        server::util::QuietLogDebug("Shutting down because of CTRL+C");
        shouldProgramRun.store(false, std::memory_order::relaxed);
        shouldProgramRunCv.notify_all();
        return TRUE;
      }

    default:
      return FALSE;
  }
}
#else

void handler(int sig, siginfo_t*, void*)
{
  if (sig == SIGTERM)
  {
    // LOA-fix (R65-1, backlog #200): обработчик сигнала делает РОВНО ОДНО —
    // выставляет флаг. Раньше он ещё писал в лог и будил условную переменную, и
    // ОБА действия не сигнало-безопасны: spdlog берёт мьютекс и выделяет память,
    // notify_all трогает внутренности condition_variable. Сигнал приходит в
    // ПРОИЗВОЛЬНЫЙ поток в ПРОИЗВОЛЬНЫЙ момент — в том числе когда прерванный
    // поток уже держит этот же мьютекс. Тогда выключение ЗАВИСАЕТ, docker stop
    // добивает SIGKILL, и запись, шедшая в этот момент, остаётся обрезанной.
    // ★Наши собственные деплой-раунды останавливают сервер постоянно, так что
    // это путь, по которому ходим чаще всех мы сами.
    shouldProgramRun.store(false, std::memory_order::relaxed);
  }
}

#endif

void InteractiveLoop()
{
  while (shouldProgramRun)
  {
    std::string commandLine;
    std::getline(std::cin, commandLine);

    const auto command = server::util::TokenizeString(
      commandLine, ' ');

    if (command[0] == "exit")
    {
      shouldProgramRun.exchange(false, std::memory_order::relaxed);
    }
  }
}

} // anon namespace

int main(int argc, char** argv)
{
  std::filesystem::path baseDirectory;

  // todo: parse arguments
  std::vector<std::string> arguments;
  for (int32_t idx = 1; idx < argc; ++idx)
  {
    arguments.emplace_back(argv[idx]);
  }

  for (const auto& argument : arguments | std::views::join_with(' '))
  {
    baseDirectory += argument;
  }

#ifdef WIN32
  // Register the control handler.
  if (SetConsoleCtrlHandler(CtrlHandler, TRUE) == FALSE)
  {
    server::util::QuietLogError(
      "Failed to set the console control handler. Windows error: 0x{:x}",
      GetLastError());
    return 1;
  };
#else
  // Register the signal action handler.
  struct sigaction act{};

  act.sa_flags = SA_SIGINFO;
  act.sa_sigaction = &handler;
  if (sigaction(SIGTERM, &act, nullptr) == -1)
  {
    server::util::QuietLogError("Failed to change the signal action handler for SIGTERM");
    return 1;
  }
#endif

  serverStartupTime = std::chrono::steady_clock::now();

  // Daily file sink.
  const auto fileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
    (baseDirectory / "logs" / "log.txt").string(), 0, 0);

  // Console sink.
  const auto consoleSink = std::make_shared<
    spdlog::sinks::stdout_color_sink_mt>();

  // Initialize the application logger with file sink and console sink.
  g_logger = std::make_shared<spdlog::logger>(
    "server",
    spdlog::sinks_init_list{fileSink, consoleSink});

  g_logger->set_level(spdlog::level::debug);
  g_logger->set_pattern("%H:%M:%S:%e [%^%l%$] [Thread %t] %v");

  // Set is as the default logger for the application.
  spdlog::set_default_logger(g_logger);

  server::util::QuietLogInfo("Running dedicated Alicia server v{}.", server::BuildVersion);
  if (not baseDirectory.empty())
    server::util::QuietLogInfo("Base directory: {}", baseDirectory.string());
  else
    server::util::QuietLogInfo("Base directory is the working directory");

  server::ServerInstance serverInstance(baseDirectory);
  // LOA (R70-fix-8, backlog #58, находка Codex 6 BLOCK-2): ★ОТКАЗ СТАРТА НА
  // СТРОГОМ КЛЮЧЕ — С ВНЯТНОЙ СТРОКОЙ, А НЕ `std::terminate`.
  // Без этого перехвата исключение из `LoadFromFile`/`LoadFromEnvironment`
  // уходило бы мимо `main` и процесс падал бы аварийно — отказ настоящий, но
  // причина в логе не названа, и оператор чинил бы «сервер крашится на старте»
  // вместо «в конфиге опечатка вот в этом ключе».
  try
  {
    serverInstance.Initialize();
  }
  catch (const server::ConfigError& x)
  {
    server::util::QuietLogError("Refusing to start: {}", x.what());
    spdlog::default_logger()->flush();
    return 1;
  }

  server::util::QuietLogInfo(
    "Server started up in {}ms",
    std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now() - serverStartupTime)
      .count());

  bool interactiveMode = true;

  #ifndef WIN32
  if (not isatty(STDIN_FILENO))
  {
    server::util::QuietLogInfo("TTY not available");
    interactiveMode = false;
  }
  #endif

  if (not interactiveMode)
  {
    server::util::QuietLogInfo("Not running in an interactive mode");

    std::mutex threadMtx;
    std::unique_lock threadLock(threadMtx);

    // LOA-fix (R65-2, backlog #200): ждём С ТАЙМАУТОМ, а не бессрочно.
    // ★Дело не только в сигнало-безопасности. Прежняя пара «флаг + notify из
    // обработчика» вместе с бессрочным wait давала ПОТЕРЮ ПРОБУЖДЕНИЯ: сигнал,
    // пришедший ПОСЛЕ проверки условия, но ДО входа в wait, будил условную
    // переменную, которую ещё никто не слушает, — и уведомление пропадало
    // насовсем. Флаг при этом уже false, но прочитать его некому: поток спит
    // вечно. Снаружи это ровно «сервер не выключается», а дальше SIGKILL.
    // С таймаутом потерянное уведомление стоит одну задержку вместо вечности,
    // и будить из обработчика больше не нужно вовсе.
    while (shouldProgramRun)
    {
      shouldProgramRunCv.wait_for(threadLock, std::chrono::milliseconds(200));
    }

    // ★Причина выключения печатается ЗДЕСЬ, в обычном потоке, а не в
    // обработчике сигнала. Диагностика сохранена, сигнальный контекст чист.
    server::util::QuietLogInfo("Shutting down, the shutdown flag was cleared");
  }
  else
  {
    InteractiveLoop();
  }

  serverInstance.Terminate();

  return 0;
}
